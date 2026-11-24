// The engine (harness/web/wasm: engine/ compiled to WebAssembly) through the standard's eight C
// functions (§3): strings in as NUL-terminated UTF-8 in the module's memory, replies out the same
// way. Works in the browser and in Node (test/).
//
// One Engine is one session: the page has one (§3.1). A text field is a handle from field().

export const ABI = { name: "bada-compatible", major: 2 };
export const HARNESS = { id: "com.teammirae.bada.web", features: ["segments", "pick", "around"] };

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/** The reply a harness sees when the engine is gone: nothing changed, and nothing is staged (§6.2). */
const panic = message => ({ ok: false, error: { code: "panic", message } });

export class Engine {
  /**
   * `wasm`: the module's bytes or a compiled WebAssembly.Module.
   * `init`: what `P_session_new` is given (§3.3): `harness`, and optionally `layouts`, `settings`
   * and `dictionary`.
   * `buffers`: the data, as named buffers (§11): `[{name, bytes}]`. Buffers the engine marks
   * `borrow` in `P_describe` are passed with P_BORROWED: they stay in the module's memory.
   *
   * Throws when the engine isn't bada-compatible v2 (§1), or gives no data or no session.
   */
  static async load(wasm, { init, buffers = [] }) {
    const module = wasm instanceof WebAssembly.Module ? wasm : await WebAssembly.compile(wasm);
    const engine = new Engine(module, init, buffers);
    engine.x = (await WebAssembly.instantiate(module, {})).exports;
    engine.start();
    return engine;
  }

  constructor(module, init, buffers) {
    this.module = module;
    this.init = init;
    this.buffers = buffers;
    this.fields = new Set();
    /** Called with a message when the engine trapped and was started again. */
    this.onrestart = null;
    this.restarts = 0;
  }

  // ---------- memory ----------

  /** Copies bytes into the module's memory. The caller frees them with `this.x.bada_web_free(ptr, len)`. */
  put(bytes) {
    const ptr = this.x.bada_web_alloc(bytes.length);
    new Uint8Array(this.x.memory.buffer, ptr, bytes.length).set(bytes);
    return ptr;
  }

  /** A string as NUL-terminated UTF-8: `[ptr, len]`, to free after the call. */
  cstr(text) {
    const bytes = encoder.encode(text + "\0");
    return [this.put(bytes), bytes.length];
  }

  /** The NUL-terminated UTF-8 string at `ptr`. Copied: a reply only lives until the next send (§3.4). */
  read(ptr) {
    if (!ptr) return null;
    const memory = new Uint8Array(this.x.memory.buffer);
    let end = ptr;
    while (memory[end]) end++;
    return decoder.decode(memory.subarray(ptr, end));
  }

  // ---------- the objects of §3.1 ----------

  /** Reads `P_describe`, builds the data and the session, and gives every field a new engine. */
  start() {
    const x = this.x;
    this.describe = JSON.parse(this.read(x.bada_describe()));
    const abi = this.describe.abi ?? {};
    // A harness refuses an engine with another major version (§1).
    if (abi.name !== ABI.name || abi.major !== ABI.major) {
      throw new Error(`this engine is ${abi.name ?? "unnamed"} ${abi.major ?? "?"}.${abi.minor ?? "?"}: the page needs ${ABI.name} ${ABI.major}`);
    }
    this.data = this.dataNew(this.buffers);
    if (!this.data) throw new Error("the engine couldn't read its data (P_data_new answered NULL)");
    const [ptr, len] = this.cstr(JSON.stringify(this.init));
    this.session = x.bada_session_new(this.data, ptr);
    x.bada_web_free(ptr, len);
    if (!this.session) throw new Error("the engine gave no session (P_session_new answered NULL)");
    for (const field of this.fields) {
      field.ptr = x.bada_new(this.session);
      if (field.selected) this.send(field, field.selected);
    }
  }

  /**
   * `P_data_new` with named buffers (§11). Each `bada_buf` is four 32-bit words in this module:
   * `name`, `ptr`, `len`, `flags`. The engine copies what isn't borrowed, so that is freed here.
   */
  dataNew(buffers) {
    const x = this.x;
    const borrow = new Set((this.describe.data ?? []).filter(d => d.borrow).map(d => d.name));
    // The table is read as 32-bit words, so it starts on a multiple of four.
    const size = buffers.length * 16 + 4;
    const raw = x.bada_web_alloc(size);
    const table = (raw + 3) & ~3;
    const freed = [[raw, size]];
    buffers.forEach(({ name, bytes }, i) => {
      const [namePtr, nameLen] = this.cstr(name);
      const ptr = this.put(bytes);
      const lent = borrow.has(name);
      freed.push([namePtr, nameLen]);
      if (!lent) freed.push([ptr, bytes.length]);
      const words = new DataView(x.memory.buffer, table + i * 16, 16);
      [namePtr, ptr, bytes.length, lent ? 1 : 0].forEach((word, j) => words.setUint32(j * 4, word, true));
    });
    const data = x.bada_data_new(table, buffers.length);
    for (const [ptr, len] of freed) x.bada_web_free(ptr, len);
    return data;
  }

  /** A new engine for one text field (`P_new`). */
  field() {
    const field = { ptr: this.x.bada_new(this.session), selected: null };
    if (!field.ptr) throw new Error("the engine gave no engine for the text field (P_new answered NULL)");
    this.fields.add(field);
    return field;
  }

  /** `P_free`. */
  free(field) {
    if (this.fields.delete(field) && this.x) this.x.bada_free(field.ptr);
    field.ptr = 0;
  }

  /** Frees the session, with its engines, and the data. */
  close() {
    for (const field of [...this.fields]) this.free(field);
    this.x.bada_session_free(this.session);
    this.x.bada_data_free(this.data);
    this.session = this.data = 0;
  }

  // ---------- events ----------

  /**
   * `P_send` with the very text, and the reply's very text. `field` is a handle from field(), or
   * null for an event of the session.
   */
  sendRaw(field, text) {
    const [ptr, len] = this.cstr(text);
    try {
      const reply = this.read(this.x.bada_send(this.session, field ? field.ptr : 0, ptr));
      this.x.bada_web_free(ptr, len);
      return reply;
    } catch (trap) {
      // WebAssembly can't unwind, so the engine's own guard never sees a panic: it traps here.
      // The module's memory can't be trusted after that. A new one takes its place, with what the
      // session and each field were last given, and the harness is told `panic` (§6.2).
      if (!(trap instanceof WebAssembly.RuntimeError) || this.restarting) throw trap;
      this.restart(trap);
      return JSON.stringify(panic(`the engine trapped (${trap.message}) and was started again`));
    }
  }

  /** One event in, one reply out, as objects. */
  send(field, event) {
    const reply = JSON.parse(this.sendRaw(field, JSON.stringify(event)));
    if (reply.ok) this.remember(field, event, reply);
    return reply;
  }

  /** Keeps what a restart must give back: each field's last select, and the session's own. */
  remember(field, event, reply) {
    if (event.type === "select" && field) field.selected = { type: "select", mode: event.mode, profile: event.profile ?? null };
    if (event.dry) return;
    if (event.type === "settings" && (event.set || event.patch)) this.init = { ...this.init, settings: reply.settings };
    if (event.type === "dictionary" && "value" in event) {
      this.init = { ...this.init, dictionary: event.value ?? undefined };
    }
    if (event.type === "layout" && "value" in event) {
      const id = event.value ? event.value.id : event.id;
      const layouts = (this.init.layouts ?? []).filter(l => l.id !== id);
      this.init = { ...this.init, layouts: event.value ? [...layouts, event.value] : layouts };
    }
  }

  restart(trap) {
    this.restarts++;
    this.restarting = true;   // a trap while starting again is not caught: the page says so
    try {
      this.x = new WebAssembly.Instance(this.module, {}).exports;
      this.start();
    } finally {
      this.restarting = false;
    }
    this.onrestart?.(String(trap?.message ?? trap));
  }
}
