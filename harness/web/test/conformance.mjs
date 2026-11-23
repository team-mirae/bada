// Replays the conformance transcripts (tools/conformance: bada-transcript/1) on the built wasm.
// Twice: raw through bada_send, checking each reply as the Rust runner does; then through the
// page's own reply-applying code on its text field model (src/field.js, the standard's §6.3),
// checking the field's text after every reply.
//   node harness/web/test/conformance.mjs [transcript.jsonl]...
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { Engine } from "../src/engine.js";
import { Field } from "../src/field.js";

const here = fileURLToPath(new URL("..", import.meta.url));
const root = fileURLToPath(new URL("../../../", import.meta.url));
const wasm = await WebAssembly.compile(readFileSync(here + "wasm/target/wasm32-unknown-unknown/release/bada_web.wasm"));
const transcripts = process.argv.length > 2 ? process.argv.slice(2)
  : ["any-engine.jsonl", "bada.jsonl"].map(f => root + "tools/conformance/transcripts/" + f);

/** Where `got` differs from what is expected of it (tools/conformance/README.md): an expected
 *  object names only the fields that matter, an array is the whole array, null also matches a
 *  missing field. */
function differs(expect, got, path = "") {
  const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);
  if (isObject(expect) && isObject(got)) {
    for (const [k, v] of Object.entries(expect)) {
      const at = path ? `${path}.${k}` : k;
      if (!(k in got)) {
        if (v === null) continue;
        return `${at}: missing, expected ${JSON.stringify(v)}`;
      }
      const d = differs(v, got[k], at);
      if (d) return d;
    }
    return null;
  }
  if (Array.isArray(expect) && Array.isArray(got)) {
    if (expect.length !== got.length) return `${path}: ${got.length} items, expected ${expect.length}`;
    for (let i = 0; i < expect.length; i++) {
      const d = differs(expect[i], got[i], `${path}[${i}]`);
      if (d) return d;
    }
    return null;
  }
  return JSON.stringify(expect) === JSON.stringify(got) ? null : `${path}: ${JSON.stringify(got)}, expected ${JSON.stringify(expect)}`;
}

/**
 * One run of a transcript. `applied`: every reply to a text field is also applied to a Field of
 * its own, and the field is checked.
 */
async function replay(path, applied) {
  const lines = readFileSync(path, "utf8").split("\n").map((text, i) => ({ text, n: i + 1 })).filter(l => l.text.trim());
  let engine = null, passed = 0, failed = 0;
  const handles = new Map();   // name → {handle, field, committed}
  const fail = (n, why) => { failed++; console.log(`  FAIL ${path}:${n}\n      ${why}`); };
  const text = event => (typeof event === "string" ? event : JSON.stringify(event));

  for (let i = 0; i < lines.length; i++) {
    const { n } = lines[i];
    let line;
    try { line = JSON.parse(lines[i].text); } catch (e) { fail(n, `not JSON: ${e.message}`); continue; }
    try {
      if ("init" in line) {
        engine?.close();
        handles.clear();
        engine = await Engine.load(wasm, { init: line.init });
      } else if ("describe" in line) {
        // P_describe needs no session: a session of its own, when the transcript has none yet.
        const describe = (engine ?? await Engine.load(wasm, { init: { harness: { id: "x.describe", version: "0" } } })).describe;
        const d = differs(line.describe, describe);
        if (d) throw new Error(d);
      } else if ("new" in line) {
        if (!engine) throw new Error("no session: a transcript starts with init");
        handles.set(line.new, { handle: engine.field(), field: new Field(), committed: "" });
      } else if ("free" in line) {
        const h = handles.get(line.free);
        if (!h) throw new Error(`no engine named ${line.free}`);
        engine.free(h.handle);
        handles.delete(line.free);
      } else if ("send" in line || "send_raw" in line) {
        if (!engine) throw new Error("no session: a transcript starts with init");
        const h = line.to == null ? null : handles.get(line.to);
        if (line.to != null && !h) throw new Error(`no engine named ${line.to}`);
        const event = "send" in line ? text(line.send) : line.send_raw;
        const reply = JSON.parse(engine.sendRaw(h ? h.handle : null, event));
        if (line.expect) {
          const d = differs(line.expect, reply);
          if (d) throw new Error(`${d}\n      reply: ${JSON.stringify(reply)}`);
        }
        if (applied && h) {
          // A line the page's own answer took is passed with this one.
          const taken = apply(h, reply, lines, i, engine);
          i += taken;
          passed += taken;
        }
      } else if (!("note" in line) && !("transcript" in line)) {
        throw new Error("a line has init, describe, new, free, send, send_raw or note");
      }
      passed++;
    } catch (e) {
      fail(n, e.message);
    }
  }
  engine?.close();
  return { passed, failed, lines: lines.length };
}

/**
 * Applies a reply to an engine's text field as the page does, and checks the field against what
 * the replies said so far: the committed text, then the marked text, and nothing else.
 *
 * When the reply asks for the selection, the page answers by itself with one `text` event. The
 * transcript's next line is that event: its selection is put in the field first, as if the user
 * had selected it, and the line is taken as sent (its expectation is checked here). Returns how
 * many lines were taken that way.
 */
function apply(h, reply, lines, i, engine) {
  let taken = 0;
  const before = { text: h.field.text, marked: h.field.marked, menu: h.field.menu };
  const commit = r => {
    if (!r.ok || !r.commit) return;
    // Nothing moves the caret in a transcript, so text is only ever added at the end; a commit
    // that replaces the selection takes the selection's place.
    if (r.replace === "selection") h.committed = h.committed.slice(0, h.committed.length - h.field.selected.length);
    h.committed += r.commit;
  };
  const answer = event => {
    const next = lines[i + 1] && JSON.parse(lines[i + 1].text);
    if (!next || next.send?.type !== "text") throw new Error(`the reply asks ${JSON.stringify(reply.ask)}, and the transcript's next line isn't its text event`);
    // The user's selection, as the transcript has it.
    if (typeof next.send.selection === "string" && !h.field.marked) {
      h.field.insert(next.send.selection);
      h.field.start = h.field.end - next.send.selection.length;
      h.committed += next.send.selection;
      event = h.field.answer(reply.ask);
    }
    const d = differs(next.send, event);
    if (d) throw new Error(`the page's answer isn't the transcript's: ${d}`);
    const answered = JSON.parse(engine.sendRaw(h.handle, JSON.stringify(event)));
    const wrong = next.expect && differs(next.expect, answered);
    if (wrong) throw new Error(`line ${lines[i + 1].n}: ${wrong}\n      reply: ${JSON.stringify(answered)}`);
    commit(answered);
    last = answered;
    taken = 1;
    return answered;
  };
  let last = reply;
  commit(reply);
  h.field.apply(reply, answer);

  const f = h.field;
  if (!reply.ok && reply.error?.code !== "panic") {
    if (f.text !== before.text || f.marked !== before.marked || f.menu !== before.menu) throw new Error("a refused event changed the field");
    return taken;
  }
  const preedit = last.preedit?.text ?? "";
  if (f.text !== h.committed + preedit) throw new Error(`the field is ${JSON.stringify(f.text)}, expected ${JSON.stringify(h.committed + preedit)}`);
  if (f.markedText !== preedit) throw new Error(`the marked text is ${JSON.stringify(f.markedText)}, expected ${JSON.stringify(preedit)}`);
  if (preedit && f.marked.start !== h.committed.length) throw new Error(`the marked text starts at ${f.marked.start}, expected ${h.committed.length}`);
  const caret = h.committed.length + (last.preedit?.caret == null ? preedit.length : Array.from(preedit).slice(0, last.preedit.caret).join("").length);
  if (f.start !== caret || f.end !== caret) throw new Error(`the caret is at ${f.start}..${f.end}, expected ${caret}`);
  const lens = f.segments.reduce((sum, s) => sum + s.len, 0);
  if (lens !== preedit.length) throw new Error(`the segments cover ${lens} of the marked text's ${preedit.length}`);
  if (f.menu.rows.length !== (last.menu?.rows?.length ?? 0)) throw new Error("the menu's rows aren't the reply's");
  return taken;
}

let failed = 0;
for (const run of ["raw", "applied"]) {
  for (const path of transcripts) {
    const r = await replay(path, run === "applied");
    failed += r.failed;
    console.log(`${r.failed ? "FAIL" : "ok  "} ${run.padEnd(7)} ${path.replace(root, "")}: ${r.passed} lines passed, ${r.failed} failed`);
  }
}
process.exit(failed ? 1 : 0);
