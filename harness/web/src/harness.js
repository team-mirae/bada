// The web harness without its page: keys in, the engine fed, replies applied to the text field
// (the standard, §6.3: `press`), the key table carried out (§9), the settings, layouts and
// dictionary read from storage and fed to the session, and the state published (§6.7).
// No DOM here: app.js binds it to the page, and test/smoke.mjs types on it as the page does.

import { Field } from "./field.js";
import { KeyTable, LANGUAGE_KEYS, MODIFIER_KEYS, PRESETS, after, presetOfKey, tableOf } from "./keytable.js";
import * as S from "./settings.js";

const NOTHING = Object.freeze({
  ok: true, commit: "", preedit: { text: "", caret: 0, segments: [] },
  menu: { rows: [], selected: null, strip: false }, staged: "", ask: null, dir: "ltr",
});

const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);
const hasError = problems => problems.some(p => p.level === "error");
/** The engine's problems, with their paths as the settings file has them. */
const under = (part, problems) => (problems ?? []).map(p => ({ ...p, path: p.path ? `${part}.${p.path}` : part }));

/** Caps Lock types the letters of a text in capitals (§6.4). A letter with no single capital stays. */
function capitals(text) {
  return Array.from(text, c => (Array.from(c.toUpperCase()).length === 1 ? c.toUpperCase() : c)).join("");
}

export class Harness {
  /**
   * - `engine`: an Engine (engine.js) whose session was started with the harness's name alone.
   * - `storage`: where the settings, layouts and dictionary are kept (localStorage, or its like).
   * - `platform`: "mac" or "pc": which preset the key table takes before a language key is seen.
   * - `language`: the language of names shown ("en", "ko", "ja").
   */
  constructor({ engine, storage, platform = "pc", language = "en" }) {
    this.engine = engine;
    this.store = new S.Store(storage);
    this.platform = platform;
    this.language = language;
    this.field = new Field();
    this.handle = engine.field();
    this.mode = "muhenkan";
    this.profile = null;       // the profile last selected, by id: never an index (§8.1)
    this.previous = null;      // the one in use before it
    this.caps = false;         // the page's own Caps Lock (§9.4)
    this.active = false;       // the text field has the keyboard
    this.reply = NOTHING;      // the last reply to the field: the engine's state (§6.1)
    this.layouts = engine.describe.layouts ?? [];
    this.problems = { settings: [], layouts: {}, dictionary: [] };
    this.dictionary = { positive: 0, negative: 0 };
    this.sawKeys = new Set();
    /** Called when anything that shows may have changed. */
    this.onchange = null;
    /** Called when a key that would enter henkan was pressed with no profile (§8.3). */
    this.ononboarding = null;
    /** Called with a sentence when something was refused and the page should say so. */
    this.onwarn = null;
    engine.onrestart = why => this.onwarn?.(`The engine stopped (${why}) and was started again. Nothing was staged.`);
    this.start();
  }

  // ---------- starting ----------

  /** Feeds the session what storage has, in the standard's order (§3.3), and selects. */
  start() {
    const layouts = this.store.json(S.STORE.layouts);
    if (layouts.problem) this.problems.layouts[""] = [{ path: "", code: "bad-value", message: layouts.problem, level: "error" }];
    for (const [id, value] of Object.entries(isObject(layouts.value) ? layouts.value : {})) {
      const reply = this.engine.send(null, { type: "layout", value });
      this.problems.layouts[id] = reply.problems ?? [];
    }
    this.readLayouts();

    // The settings: what storage has, or a new install's. Text that can't be used changes nothing
    // (§8.2): it stays in storage for the editor to show, and the engine keeps its defaults.
    this.file = S.fresh(this.engine.describe);
    this.effective = this.engine.send(null, { type: "settings" }).settings ?? this.file.engine;
    this.broken = false;
    const stored = this.store.text(S.STORE.settings);
    if (stored != null) {
      const loaded = this.loadSettings(stored, false);
      this.broken = !loaded.ok;
    }
    this.seen = this.store.text(S.STORE.seen);
    if (!PRESETS[this.seen]) this.seen = null;
    this.retable();

    const dictionary = this.store.json(S.STORE.dictionary);
    if (dictionary.problem) this.problems.dictionary = [{ path: "", code: "bad-value", message: dictionary.problem, level: "error" }];
    else if (dictionary.value !== undefined) this.feedDictionary(dictionary.value);

    // A new field is told its mode and profile (§3.1): the ones the page was last left in.
    const last = this.store.json(S.STORE.last).value;
    const has = id => this.profiles.some(p => p.id === id);
    const profile = isObject(last) && has(last.profile) ? last.profile : null;
    this.previous = isObject(last) && has(last.previous) ? last.previous : null;
    this.profile = profile;
    if (!(isObject(last) && last.mode === "henkan" && profile && this.select("henkan", profile))) this.select("muhenkan", profile);
  }

  // ---------- what the harness knows ----------

  /** The henkan profiles in effect: `[{id, name, layouts}]`, in display order. */
  get profiles() {
    return this.effective?.henkan?.profiles ?? [];
  }

  /** The key preset in use when the settings have no bindings of their own (§9.5). */
  get preset() {
    const chosen = S.own(this.file).preset;
    return chosen === "auto" ? this.seen ?? (this.platform === "mac" ? "jis-mac" : "jis-pc") : chosen;
  }

  /** The harness's state, as the key table reads it (§9.1). */
  state() {
    return {
      mode: this.mode, profile: this.profile, previous: this.previous,
      menu_rows: this.reply.menu?.rows?.length ?? 0, staged: this.reply.staged ?? "", caps_lock: this.caps,
    };
  }

  retable() {
    this.table = tableOf(this.file.keys, this.preset);
    if (this.keys) this.keys.set(this.table, this.profiles);
    else this.keys = new KeyTable(this.table, this.profiles);
  }

  /** A layout's name in the page's language. */
  nameOf(id) {
    const layout = this.layouts.find(l => l.id === id);
    return layout ? layout.name?.[this.language] ?? layout.name?.en ?? id : id;
  }

  changed() {
    this.onchange?.();
  }

  // ---------- the text field ----------

  /** One event to the field's engine, and its reply applied (§6.3). An `ask` is answered once. */
  send(event) {
    const one = e => {
      const reply = this.engine.send(this.handle, e);
      if (reply.ok) this.reply = reply;
      else if (reply.error?.code === "panic") this.reply = NOTHING;
      else this.onwarn?.(`The engine refused ${e.type}${e.name ? " " + e.name : ""}: ${reply.error?.code} (${reply.error?.message ?? ""})`);
      return reply;
    };
    const reply = one(event);
    this.field.apply(reply, one);
    return reply;
  }

  /** Tells the engine the mode and the profile (§5). Returns whether it took them. */
  select(mode, profile) {
    const reply = this.send({ type: "select", mode, profile: profile ?? null });
    if (!reply.ok) return false;
    const next = after(this.state(), { then: "select", mode, profile: profile ?? null });
    [this.mode, this.profile, this.previous] = [next.mode, next.profile, next.previous];
    // transient: the lock is turned off when muhenkan is left (§9.4).
    if (mode === "henkan" && this.table.caps_lock === "transient") this.caps = false;
    this.store.write(S.STORE.last, JSON.stringify({ mode: this.mode, profile: this.profile, previous: this.previous }));
    this.changed();
    return true;
  }

  /** Commits what is staged: the field loses focus, is clicked, or is edited by the app (§5). */
  flush(cause = "harness") {
    if (this.field.marked || this.reply.staged || this.reply.menu?.rows?.length) {
      this.send({ type: "flush", cause });
      this.changed();
    }
  }

  /** A click on a row of the menu: the row is picked, then committed (§5). */
  pick(row) {
    if (this.send({ type: "pick", row }).ok) this.send({ type: "flush", cause: "harness" });
    this.changed();
  }

  /** The mode's own button: to muhenkan, or to henkan with a profile. No profile: the onboarding. */
  enter(mode, profile = null) {
    if (mode === "muhenkan") this.select("muhenkan", this.profile);
    else if (!this.profiles.length) this.ononboarding?.();
    else this.select("henkan", profile ?? decideEntered(this));
  }

  // ---------- keys ----------

  /**
   * A key went down. `press`: `{code, ctrl, alt, shift, meta, repeat}`.
   * `osKey`: what the OS's own layout would type for it (`KeyboardEvent.key`), or null.
   * `synthetic`: no browser is behind this press (a click on the drawn keyboard, a test), so the
   * text field model does what the app would.
   * Returns true when the press was dealt with here, and false when the browser should have it.
   */
  press(press, { osKey = null, synthetic = false } = {}) {
    const { code } = press;
    this.see(code);
    let dealt = !MODIFIER_KEYS.includes(code);
    this.keys.down(press, () => this.state(), outcome => {
      const done = this.run(outcome, { osKey, synthetic });
      // A key that waited for its pair runs before this one: only this one's answer is the browser's.
      if (outcome.press === press) dealt = done;
    });
    this.changed();
    return dealt;
  }

  /** A key went up: a pair, a key of a bound pair, or a bound modifier may run now (§9.3). */
  release(code) {
    this.keys.up(code, () => this.state(), outcome => this.run(outcome, { synthetic: true, late: true }));
    this.changed();
  }

  /** The page lost the keyboard: no key is down any more. */
  drop() {
    this.keys.reset();
  }

  /** A language key the page sees says which keyboard this is (§9.5), when the preset is automatic. */
  see(code) {
    if (LANGUAGE_KEYS.includes(code) || code === "CapsLock") this.sawKeys.add(code);
    const preset = presetOfKey(code);
    if (!preset || preset === this.seen) return;
    this.seen = preset;
    this.store.write(S.STORE.seen, preset);
    if (S.own(this.file).preset === "auto" && !Array.isArray(this.file.keys?.bindings)) this.retable();
  }

  /** Carries out what the key table decided (§9.2). Returns whether the press was dealt with. */
  run(outcome, { osKey = null, synthetic = false, late = false } = {}) {
    this.caps = !!outcome.caps_lock;
    switch (outcome.then) {
      case "select": this.select(outcome.mode, outcome.profile); return true;
      case "action": this.send({ type: "action", name: outcome.name }); return true;
      case "onboarding": this.ononboarding?.(); return true;
      case "type": this.flush(); this.field.insert(outcome.text); return true;
      case "swallow": return true;
      case "pass": return false;
      case "key": return late ? true : this.key(outcome.press, { osKey, synthetic });
    }
    return true;
  }

  /** A press the table doesn't take: the engine reads it, and the app gets it if the engine doesn't (§6.3). */
  key(press, { osKey = null, synthetic = false } = {}) {
    const event = { type: "key", key: press.code, shift: !!press.shift, ctrl: !!press.ctrl, alt: !!press.alt, meta: !!press.meta };
    if (press.repeat) event.repeat = true;
    const reply = this.send(event);
    if (!reply.ok || reply.consumed) return true;
    return this.pass(press, reply.types ?? null, { osKey, synthetic });
  }

  /**
   * A key the engine passed on (§6.4). With `types` the key types that text, whatever layout the
   * OS has: the browser types it when it would type the same, and otherwise the page does.
   */
  pass(press, types, { osKey = null, synthetic = false } = {}) {
    // A shortcut is the app's, as the physical key.
    if (press.ctrl || press.meta) return synthetic;
    if (typeof types === "string") {
      const text = this.caps ? capitals(types) : types;
      if (!synthetic && osKey === text) return false;
      this.field.insert(text);
      return true;
    }
    // No say from the engine: the app has the key as the OS has it. A text field types Space and Enter.
    if (!press.alt && press.code === "Space") { this.field.insert(" "); return true; }
    if (!press.alt && press.code === "Enter") { this.field.insert("\n"); return true; }
    if (!synthetic) return false;
    this.field.key(press.code, press);
    return true;
  }

  // ---------- settings (§8.2, §10.1) ----------

  /** The settings file as the editor shows it: what storage has, readable or not, or a new install's. */
  get settingsText() {
    return this.store.text(S.STORE.settings) ?? S.print(this.file);
  }

  /**
   * Checks a settings file's text, and takes it unless `dry`: its `engine` part goes to the
   * engine, its `keys` become the key table, and it is kept. Any error-level problem changes
   * nothing (§8.2). Returns `{ok, problems}`.
   */
  loadSettings(text, keep = true, dry = false) {
    const result = this.readSettings(text, dry);
    if (!dry) this.problems.settings = result.problems;
    if (!result.ok || dry) return result;
    this.file = result.file;
    this.effective = result.effective;
    this.broken = false;
    if (keep) {
      const failed = this.store.write(S.STORE.settings, text);
      if (failed) result.problems.push({ path: "", code: "not-kept", message: failed, level: "warning" });
    }
    if (this.keys) this.settled();
    return result;
  }

  readSettings(text, dry) {
    let file;
    try { file = JSON.parse(text); } catch (e) {
      return { ok: false, problems: [{ path: "", code: "bad-json", message: `the settings aren't JSON: ${e.message}`, level: "error" }] };
    }
    if (!isObject(file)) return { ok: false, problems: S.check(file) };
    // Every part is checked before anything is taken, so that every problem shows at once and a
    // file with one wrong part changes nothing: the engine checks its own with a dry run, and the
    // harness the rest.
    const part = isObject(file.engine) ? file.engine : {};
    const tried = this.engine.send(null, { type: "settings", set: part, dry: true });
    const engine = under("engine", tried.problems);
    if (!tried.ok && !hasError(engine)) {
      engine.push({ path: "engine", code: tried.error?.code ?? "invalid", message: tried.error?.message ?? "the engine refused the settings", level: "error" });
    }
    const named = tried.ok ? tried.settings?.henkan?.profiles : part.henkan?.profiles;
    const profiles = (Array.isArray(named) ? named : []).filter(isObject);
    const problems = [...S.check(file, profiles, this.layouts), ...engine];
    if (hasError(problems) || dry) return { ok: !hasError(problems), problems, file };
    const taken = this.engine.send(null, { type: "settings", set: part });
    if (!taken.ok) return { ok: false, problems: under("engine", taken.problems) };
    return { ok: true, problems, file, effective: taken.settings };
  }

  /** After the settings changed: the table is read again, and a profile that is gone is left. */
  settled() {
    this.retable();
    const has = id => this.profiles.some(p => p.id === id);
    if (!has(this.previous)) this.previous = null;
    // A field whose profile was changed commits at its next event (§8.2): this is that event.
    if (this.mode === "henkan" && !has(this.profile)) {
      this.profile = null;
      this.select("muhenkan", null);
    } else {
      if (!has(this.profile)) this.profile = null;
      this.send({ type: "flush" });
    }
    this.changed();
  }

  /** Changes one of this page's own settings (settings.js OWN), through the same door. */
  setOwn(name, value) {
    if (this.broken) return this.refuseBroken();
    return this.loadSettings(S.print(S.withOwn(this.file, name, value)));
  }

  /** Easy mode of the onboarding (§8.3): the muhenkan layout and one henkan layout. */
  easy(muhenkan, henkan) {
    if (this.broken) return this.refuseBroken();
    return this.loadSettings(S.print(S.withEasy(this.file, muhenkan, henkan, this.nameOf(henkan))));
  }

  refuseBroken() {
    const message = "the settings in storage can't be used: fix them in Settings first, or they would be replaced";
    return { ok: false, problems: [{ path: "", code: "bad-json", message, level: "error" }] };
  }

  // ---------- layouts (§7.6) and the dictionary (§11) ----------

  readLayouts() {
    const reply = this.engine.send(null, { type: "layout" });
    if (reply.ok && reply.layouts) this.layouts = reply.layouts;
  }

  /** The user's layouts as storage has them: `{id: layout}`. */
  get customLayouts() {
    const kept = this.store.json(S.STORE.layouts).value;
    return isObject(kept) ? kept : {};
  }

  /**
   * Checks a layout file's text, and takes it unless `dry`: a new layout, or one that replaces a
   * layout with its id. The reply's warnings are what §7.7 asks to be shown before it is saved.
   */
  loadLayout(text, dry = false) {
    let value;
    try { value = JSON.parse(text); } catch (e) {
      return { ok: false, problems: [{ path: "", code: "bad-json", message: `the layout isn't JSON: ${e.message}`, level: "error" }] };
    }
    if (!isObject(value)) return { ok: false, problems: [{ path: "", code: "bad-value", message: "a layout is a JSON object", level: "error" }] };
    const reply = this.engine.send(null, { type: "layout", value, dry });
    const result = { ok: !!reply.ok, problems: reply.problems ?? [], id: value.id };
    if (!reply.ok && !result.problems.length) result.problems.push({ path: "", code: reply.error?.code ?? "invalid", message: reply.error?.message ?? "the engine refused the layout", level: "error" });
    if (!reply.ok || dry) return result;
    const failed = this.store.write(S.STORE.layouts, JSON.stringify({ ...this.customLayouts, [value.id]: value }));
    if (failed) result.problems.push({ path: "", code: "not-kept", message: failed, level: "warning" });
    this.problems.layouts[value.id] = reply.problems ?? [];
    this.layouts = reply.layouts ?? this.layouts;
    this.send({ type: "flush" });
    this.changed();
    return result;
  }

  /** Removes one of the user's layouts. A stock layout it replaced comes back. */
  removeLayout(id) {
    const kept = this.customLayouts;
    const reply = this.engine.send(null, { type: "layout", id, value: null });
    // A layout the engine never took (it had errors) is only in storage.
    if (!reply.ok && !(id in kept)) return { ok: false, problems: reply.problems ?? [] };
    delete kept[id];
    delete this.problems.layouts[id];
    this.store.write(S.STORE.layouts, Object.keys(kept).length ? JSON.stringify(kept) : null);
    this.readLayouts();
    this.send({ type: "flush" });
    this.changed();
    return { ok: true, problems: [] };
  }

  /** The user dictionary as storage has it, as text. */
  get dictionaryText() {
    return this.store.text(S.STORE.dictionary) ?? "";
  }

  feedDictionary(value) {
    const reply = this.engine.send(null, { type: "dictionary", value });
    this.problems.dictionary = reply.problems ?? [];
    if (!reply.ok && !this.problems.dictionary.length) {
      this.problems.dictionary = [{ path: "", code: reply.error?.code ?? "invalid", message: reply.error?.message ?? "the engine refused the dictionary", level: "error" }];
    }
    if (reply.dictionary) this.dictionary = reply.dictionary;
    return { ok: !!reply.ok, problems: this.problems.dictionary };
  }

  /** Sets the user dictionary from a file's text, or clears it with "". */
  loadDictionary(text) {
    let value = null;
    if (text.trim()) {
      try { value = JSON.parse(text); } catch (e) {
        return { ok: false, problems: [{ path: "", code: "bad-json", message: `the dictionary isn't JSON: ${e.message}`, level: "error" }] };
      }
      if (!isObject(value)) return { ok: false, problems: [{ path: "", code: "bad-value", message: "a dictionary is a JSON object", level: "error" }] };
    }
    const result = this.feedDictionary(value);
    if (result.ok) this.store.write(S.STORE.dictionary, value ? text : null);
    this.changed();
    return result;
  }

  // ---------- the published state (§6.7) ----------

  /** `bada-compatible-state/2`: the last reply's display fields, and the harness's own. */
  published() {
    const r = this.reply;
    const available = this.platform === "mac" ? ["Backquote", "Lang2", "Lang1", "CapsLock"] : ["Backquote", "NonConvert", "Convert", "KanaMode", "CapsLock"];
    for (const code of this.sawKeys) if (!available.includes(code)) available.push(code);
    const problems = [
      ...this.problems.settings.map(p => ({ ...p, in: "settings" })),
      ...Object.entries(this.problems.layouts).flatMap(([id, list]) => list.map(p => ({ ...p, in: `layouts/${id}` }))),
      ...this.problems.dictionary.map(p => ({ ...p, in: "dictionary" })),
    ];
    return {
      format: "bada-compatible-state/2",
      mode: this.mode, profile: this.profile, staged: r.staged ?? "",
      preedit: { text: r.preedit?.text ?? "", caret: r.preedit?.caret ?? Array.from(r.preedit?.text ?? "").length, segments: r.preedit?.segments ?? [] },
      menu: { rows: r.menu?.rows ?? [], selected: r.menu?.selected ?? null, strip: !!r.menu?.strip },
      ask: r.ask ?? null, dir: r.dir === "rtl" ? "rtl" : "ltr",
      harness: {
        id: this.engine.init.harness.id, version: this.engine.init.harness.version, app: null, secure: false, active: this.active,
        caps_lock: { on: this.caps, setting: this.table.caps_lock, rule: this.table.caps_rule },
        keys: { available, chords: true, missing: [], preset: this.table.preset },
        problems,
      },
    };
  }
}

/** The profile henkan enters from the mode's own button: the one last used, or the first. */
function decideEntered(harness) {
  const has = id => harness.profiles.some(p => p.id === id);
  const named = harness.table.henkan_enters;
  if (named !== "last" && has(named)) return named;
  return has(harness.profile) ? harness.profile : harness.profiles[0].id;
}
