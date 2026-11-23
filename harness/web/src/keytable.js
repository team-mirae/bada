// The key table (the standard, §9): what language keys, Caps Lock and bound chords do.
// A pure module: no DOM, no engine. It reads a table, a profile list and the harness's state, and
// says what must happen. The harness does it (harness.js). test/keytable.mjs replays the
// standard's cases (docs/cases/key-table.json) on it.

/** The language keys (§9.3). Caps Lock is not one of them: it can't be in a pair. */
export const LANGUAGE_KEYS = ["Backquote", "NonConvert", "Convert", "KanaMode", "Lang2", "Lang1"];
/** Keys that are modifiers. One pressed and released alone can have a binding (§9.3). */
export const MODIFIER_KEYS = [
  "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight",
];
const MODS = ["ctrl", "alt", "shift", "meta"];
const isLanguage = code => LANGUAGE_KEYS.includes(code);
const isModifier = code => MODIFIER_KEYS.includes(code);

/** The presets of §9.5: the bindings of a settings file that has no `keys.bindings`. */
export const PRESETS = {
  "jis-mac": [
    { on: "Lang2", do: ["mode:muhenkan"] },
    { on: "Lang1", do: ["profile.first"] },
    { on: "Lang1", do: ["menu.next", "profile.first"], when: "henkan" },
    { on: "shift+Lang1", do: ["menu.prev"], when: "henkan" },
    { on: "ctrl+Lang1", do: ["fix"], when: "henkan" },
    { on: "Backquote", do: ["width.full"] },
    { on: "shift+Backquote", do: ["width.half"] },
  ],
  "jis-pc": [
    { on: "NonConvert", do: ["mode:muhenkan"] },
    { on: "Convert", do: ["profile.first"] },
    { on: "Convert", do: ["menu.next", "profile.first"], when: "henkan" },
    { on: "shift+Convert", do: ["menu.prev"], when: "henkan" },
    { on: "ctrl+Convert", do: ["fix"], when: "henkan" },
    { on: "Backquote", do: ["width.full"] },
    { on: "shift+Backquote", do: ["width.half"] },
  ],
  none: [{ on: "CapsLock", do: ["mode.toggle"] }],
};
export const PRESET_NAMES = { "jis-mac": "JIS on a Mac", "jis-pc": "JIS on a PC", none: "No language keys" };

/** The preset for a language key the page has seen, or null. */
export function presetOfKey(code) {
  if (code === "Lang1" || code === "Lang2") return "jis-mac";
  if (code === "Convert" || code === "NonConvert" || code === "KanaMode") return "jis-pc";
  return null;
}

const ID = /^[a-z0-9-]{1,32}$/;
const CODE = /^[A-Z][A-Za-z0-9]*$/;
const ACTION = /^(mode\.toggle|mode:(muhenkan|henkan)|profile:[a-z0-9-]{1,32}|profile\.(next|prev|last|first)|menu\.(next|prev)|fix|width\.(full|half)|[a-z]{2,3}(-[a-z0-9]+)+\.[a-z0-9]+(-[a-z0-9]+)*|pass|none)$/;
const WHEN = /^(muhenkan|henkan|profile:[a-z0-9-]{1,32})$/;

/**
 * A binding's `on`, or a case's `press`: `{code, ctrl, alt, shift, meta}`, or `{pair: [a, b]}` for
 * two language keys. null: not a key the table can be on.
 */
export function parseOn(on) {
  if (typeof on !== "string") return null;
  const parts = on.split("+");
  const code = parts.pop();
  if (!CODE.test(code)) return null;
  if (parts.length === 1 && CODE.test(parts[0])) {
    const [a, b] = [parts[0], code];
    return isLanguage(a) && isLanguage(b) && a !== b ? { pair: [a, b] } : null;
  }
  const press = { code, ctrl: false, alt: false, shift: false, meta: false };
  for (const mod of parts) {
    if (!MODS.includes(mod) || press[mod]) return null;
    press[mod] = true;
  }
  return press;
}

/** One name for a press, whatever order it was written in: `ctrl+alt+shift+meta+Code`, or a sorted pair. */
export function nameOf(press) {
  if (press.pair) return [...press.pair].sort().join("+");
  return [...MODS.filter(mod => press[mod]), press.code].join("+");
}

/**
 * The table in effect, from the `keys` part of a settings file: its bindings, or the preset's when
 * it has none, and the defaults of the other fields (§9.5). `preset` is a name of PRESETS.
 */
export function tableOf(keys = {}, preset = "none") {
  const bindings = Array.isArray(keys.bindings) ? keys.bindings : PRESETS[preset] ?? PRESETS.none;
  return {
    bindings: bindings.map(b => {
      const press = parseOn(b?.on);
      return {
        on: b?.on, do: Array.isArray(b?.do) ? b.do : [], when: b?.when ?? null,
        name: press && nameOf(press), pair: press?.pair ?? null,
      };
    }),
    preset: Array.isArray(keys.bindings) ? null : preset,
    cycle: Array.isArray(keys.cycle) ? keys.cycle : [],
    henkan_enters: typeof keys.henkan_enters === "string" ? keys.henkan_enters : "last",
    caps_lock: keys.caps_lock === "transient" ? "transient" : "respect",
    caps_rule: keys.caps_rule === "upper_snake" ? "upper_snake" : "allcaps",
  };
}

/** How specific a binding's `when` is, when it holds: 3 a profile's, 2 a mode's, 1 none. 0: it doesn't hold. */
function holds(when, state) {
  if (when == null) return 1;
  if (when === "muhenkan" || when === "henkan") return state.mode === when ? 2 : 0;
  if (when.startsWith("profile:")) return state.mode === "henkan" && state.profile === when.slice(8) ? 3 : 0;
  return 0;
}

/** The binding for a press: the most specific that holds, and among equals the later (§9.1). -1: none. */
function bindingFor(table, state, name) {
  let best = -1, rank = 0;
  table.bindings.forEach((b, i) => {
    if (b.name !== name) return;
    const r = holds(b.when, state);
    if (r && r >= rank) [best, rank] = [i, r];
  });
  return best;
}

/** The profile that `mode:henkan` and the toggle enter (§9.1). */
function entered(table, profiles, state) {
  const has = id => profiles.some(p => p.id === id);
  if (table.henkan_enters !== "last") return has(table.henkan_enters) ? table.henkan_enters : profiles[0].id;
  return state.profile != null && has(state.profile) ? state.profile : profiles[0].id;
}

/** What one action does now (§9.2). null: it doesn't apply, and the next of the list is tried. */
function act(action, table, profiles, state) {
  const henkan = state.mode === "henkan";
  const has = id => profiles.some(p => p.id === id);
  const onboarding = { then: "onboarding" };
  const muhenkan = { then: "select", mode: "muhenkan", profile: state.profile ?? null };
  // To a profile, entering henkan if needed. It changes nothing when that profile is in use.
  const to = id => (henkan && state.profile === id ? null : { then: "select", mode: "henkan", profile: id });
  const send = name => ({ then: "action", name });
  switch (action) {
    case "mode.toggle":
      if (henkan) return muhenkan;
      return profiles.length ? to(entered(table, profiles, state)) : onboarding;
    case "mode:muhenkan": return henkan ? muhenkan : null;
    case "mode:henkan":
      if (henkan) return null;
      return profiles.length ? to(entered(table, profiles, state)) : onboarding;
    case "menu.next": case "menu.prev": return state.menu_rows > 1 ? send(action) : null;
    case "fix": return state.staged ? send(action) : null;
    case "width.full": case "width.half": return send(action);
    case "pass": return { then: "pass" };
    case "none": return { then: "swallow" };
  }
  if (action.startsWith("profile:") || action.startsWith("profile.")) {
    // With no profile at all, any action that would enter henkan opens the onboarding (§9.2).
    if (!profiles.length) return onboarding;
    if (action.startsWith("profile:")) return has(action.slice(8)) ? to(action.slice(8)) : null;
    if (action === "profile.first") return to(profiles[0].id);
    if (action === "profile.last") {
      const last = henkan ? state.previous : state.profile;
      return last != null && has(last) ? to(last) : null;
    }
    // next, prev: through the cycle's profiles that exist, from the one last selected.
    const cycle = table.cycle.filter(has);
    if (!cycle.length) return null;
    const at = cycle.indexOf(state.profile);
    const n = cycle.length;
    if (action === "profile.next") return to(at < 0 ? cycle[0] : cycle[(at + 1) % n]);
    if (action === "profile.prev") return to(at < 0 ? cycle[n - 1] : cycle[(at + n - 1) % n]);
    return null;
  }
  // <layout id>.<action id>: in henkan, when the layout is a candidate of the profile in use.
  const dot = action.indexOf(".");
  if (dot > 0) {
    const profile = henkan ? profiles.find(p => p.id === state.profile) : null;
    return profile && (profile.layouts ?? []).includes(action.slice(0, dot)) ? send(action) : null;
  }
  return null;
}

/**
 * What one press comes to (§9.1, §9.2, §9.4).
 *
 * - `table`: from tableOf. `profiles`: `engine.henkan.profiles`.
 * - `state`: `{mode, profile, previous, menu_rows, staged, caps_lock}`: the profile last selected
 *   and the one before it, and the last reply's menu rows and staged text.
 * - `press`: `{code, ctrl, alt, shift, meta}`, `{pair: [a, b]}`, or either as a string.
 *
 * Returns `{then, binding, ran, caps_lock, press}` and, by `then`: `select` (`mode`, `profile`),
 * `action` (`name`), `type` (`text`), `key` (the press is fed to the engine), `pass` (the app or
 * the OS has the key), `swallow`, `onboarding`. `caps_lock` is the lock afterwards.
 */
export function decide(table, profiles, state, press) {
  if (typeof press === "string") press = parseOn(press);
  const lock = !!state.caps_lock;
  const out = o => ({ binding: null, ran: null, caps_lock: lock, press, ...o });
  const binding = bindingFor(table, state, nameOf(press));
  if (binding >= 0) {
    for (const action of table.bindings[binding].do) {
      const done = act(action, table, profiles, state);
      if (!done) continue;
      // transient: the lock is turned off when muhenkan is left (§9.4).
      const off = done.then === "select" && done.mode === "henkan" && table.caps_lock === "transient";
      return out({ ...done, binding, ran: action, caps_lock: off ? false : lock });
    }
    return out({ then: "swallow", binding });
  }
  // Not the table's. Caps Lock unbound is the lock, with any modifiers (§9.4).
  if (press.code === "CapsLock") {
    if (table.caps_lock === "transient" && state.mode === "henkan") return out({ then: "swallow", caps_lock: false });
    return out({ then: "pass", caps_lock: !lock });
  }
  // A language key never goes to the engine: it goes to the OS as it is (§9.1).
  if (press.pair || isLanguage(press.code)) return out({ then: "pass" });
  // UPPER_SNAKE: with the lock on in muhenkan, Space types _ and ⌥Space a space (§9.4).
  if (table.caps_rule === "upper_snake" && lock && state.mode === "muhenkan" && press.code === "Space"
      && !press.ctrl && !press.meta) {
    return out({ then: "type", text: press.alt ? " " : "_" });
  }
  return out({ then: "key" });
}

/** The harness's state after an outcome: `{mode, profile, previous, caps_lock}` change, the rest stays. */
export function after(state, outcome) {
  const next = { ...state, caps_lock: outcome.caps_lock ?? state.caps_lock };
  if (outcome.then !== "select") return next;
  next.mode = outcome.mode;
  // The profile becomes the one selected, and the one before it is what profile.last goes back to (§9.2).
  if (outcome.mode === "henkan" && outcome.profile !== state.profile) {
    next.previous = state.profile ?? null;
    next.profile = outcome.profile;
  }
  return next;
}

/**
 * The table over time: presses and releases in order (§9.3). A key of a bound pair runs when it is
 * released, a pair when both its keys are up, and a bound modifier when it was pressed and released
 * alone.
 *
 * `down` and `up` take `state`, a function that returns the harness's state as it is now, and
 * `emit`, which is called with each outcome of `decide`, in order, and carries it out before it
 * returns: the next outcome is decided on the state it leaves.
 */
export class KeyTable {
  constructor(table = tableOf(), profiles = []) {
    this.set(table, profiles);
  }

  set(table, profiles) {
    this.table = table;
    this.profiles = profiles;
    this.reset();
  }

  /** Forgets every key that is down: the page lost the keyboard. */
  reset() {
    this.waiting = null;      // a key of a bound pair that is down alone
    this.pair = null;         // {keys: [a, b], up: Set}: both keys of a bound pair went down
    this.tap = null;          // a bound modifier that is down alone
    this.held = new Set();    // the keys that are down
  }

  /** The index of the binding on a pair that holds now, or -1. */
  pairBinding(a, b, state) {
    return bindingFor(this.table, state, nameOf({ pair: [a, b] }));
  }

  /** Whether `code` is a key of a pair that has a binding that holds now. */
  inBoundPair(code, state) {
    return this.table.bindings.some(b => b.pair?.includes(code) && holds(b.when, state));
  }

  /** `press`: `{code, ctrl, alt, shift, meta, repeat}`. */
  down(press, state, emit) {
    const { code } = press;
    const decideNow = p => decide(this.table, this.profiles, state(), p);
    if (press.repeat && this.held.has(code)) {
      // A held key repeats only what isn't the table's: a binding runs once, but for pass.
      if (isModifier(code)) return;
      const swallow = { then: "swallow", binding: null, ran: null, press, caps_lock: !!state().caps_lock };
      const again = this.waiting === code || this.pair?.keys.includes(code) ? swallow : decideNow(press);
      emit(["key", "pass", "type"].includes(again.then) ? again : swallow);
      return;
    }
    const bare = !press.ctrl && !press.alt && !press.shift && !press.meta;
    // Another key: a pair that is down is no pair, and a key that waited for one runs now.
    if (this.pair && !this.pair.keys.includes(code)) {
      this.pair = null;
    } else if (this.pair) {
      this.pair.up.delete(code);
      return;
    } else if (this.waiting && this.waiting !== code) {
      if (bare && isLanguage(code) && this.pairBinding(this.waiting, code, state()) >= 0) {
        this.pair = { keys: [this.waiting, code], up: new Set() };
        this.waiting = null;
        this.held.add(code);
        return;
      }
      const waited = this.waiting;
      this.waiting = null;
      emit(decideNow({ code: waited, ctrl: false, alt: false, shift: false, meta: false }));
    }
    if (isModifier(code)) {
      // A modifier isn't a key press of its own. Alone, pressed and released, it may have a binding.
      this.tap = bindingFor(this.table, state(), code) >= 0 ? code : null;
      this.held.add(code);
      return;
    }
    this.tap = null;
    if (bare && isLanguage(code) && this.inBoundPair(code, state())) {
      this.waiting = code;
      this.held.add(code);
      return;
    }
    this.held.add(code);
    emit(decideNow(press));
  }

  up(code, state, emit) {
    this.held.delete(code);
    const bare = { code, ctrl: false, alt: false, shift: false, meta: false };
    if (this.tap === code) {
      this.tap = null;
      const outcome = decide(this.table, this.profiles, state(), bare);
      if (outcome.binding != null) emit(outcome);
    } else if (this.pair?.keys.includes(code)) {
      this.pair.up.add(code);
      if (this.pair.up.size < 2) return;
      const pair = this.pair.keys;
      this.pair = null;
      emit(decide(this.table, this.profiles, state(), { pair }));
    } else if (this.waiting === code) {
      this.waiting = null;
      emit(decide(this.table, this.profiles, state(), bare));
    }
  }
}

/**
 * What is wrong with the `keys` part of a settings file, as the engine reports its own part (§6.6):
 * `[{path, code, message, level}]`. An error is a table the harness can't read; a warning is a
 * binding that can never do anything. `profiles` and `layouts` are what the settings and the
 * session have: `[{id, layouts}]` and `[{id, actions: [{id}]}]`.
 */
export function check(keys, profiles = [], layouts = []) {
  const problems = [];
  const problem = level => (path, code, message) => problems.push({ path, code, message, level });
  const error = problem("error"), warning = problem("warning");
  if (keys === undefined) return problems;
  if (!keys || typeof keys !== "object" || Array.isArray(keys)) {
    error("keys", "bad-value", "keys is an object");
    return problems;
  }
  const has = id => profiles.some(p => p.id === id);
  for (const name of Object.keys(keys)) {
    if (!["bindings", "cycle", "henkan_enters", "caps_lock", "caps_rule"].includes(name)) {
      error(`keys.${name}`, "unknown-field", `keys has no field ${name}`);
    }
  }
  if (keys.bindings !== undefined && !Array.isArray(keys.bindings)) error("keys.bindings", "bad-value", "bindings is a list");
  (Array.isArray(keys.bindings) ? keys.bindings : []).forEach((b, i) => {
    const at = `keys.bindings[${i}]`;
    if (!b || typeof b !== "object" || Array.isArray(b)) return error(at, "bad-value", "a binding is an object");
    for (const name of Object.keys(b)) {
      if (!["on", "do", "when"].includes(name)) error(`${at}.${name}`, "unknown-field", `a binding has no field ${name}`);
    }
    const press = parseOn(b.on);
    if (!press) {
      error(`${at}.on`, "bad-value", "on is one key with ctrl, alt, shift and meta before it, or two different language keys");
    } else if (!press.pair && b.on !== nameOf(press)) {
      error(`${at}.on`, "bad-value", `the modifiers are written in the order ctrl, alt, shift, meta: ${nameOf(press)}`);
    }
    if (!Array.isArray(b.do) || !b.do.length) error(`${at}.do`, "bad-value", "do is a list of at least one action");
    (Array.isArray(b.do) ? b.do : []).forEach((action, j) => {
      const path = `${at}.do[${j}]`;
      if (typeof action !== "string" || !ACTION.test(action)) return error(path, "unknown-action", `no action is written ${JSON.stringify(action)}`);
      if (action.startsWith("profile:") && !has(action.slice(8))) {
        warning(path, "unknown-profile", `no profile named ${action.slice(8)}: this action never applies`);
      }
      const dot = action.indexOf(".");
      if (dot > 0 && action.slice(0, dot).includes("-")) {
        const layout = layouts.find(l => l.id === action.slice(0, dot));
        if (!layout) warning(path, "unknown-layout", `no layout named ${action.slice(0, dot)}`);
        else if (!(layout.actions ?? []).some(a => a.id === action.slice(dot + 1))) {
          warning(path, "unknown-action", `${layout.id} has no action ${action.slice(dot + 1)}`);
        }
      }
    });
    if (b.when !== undefined) {
      if (typeof b.when !== "string" || !WHEN.test(b.when)) error(`${at}.when`, "bad-value", "when is muhenkan, henkan or profile:<id>");
      else if (b.when.startsWith("profile:") && !has(b.when.slice(8))) {
        warning(`${at}.when`, "unknown-profile", `no profile named ${b.when.slice(8)}: this binding never holds`);
      }
    }
  });
  if (keys.cycle !== undefined) {
    if (!Array.isArray(keys.cycle) || keys.cycle.some(id => typeof id !== "string" || !ID.test(id))) {
      error("keys.cycle", "bad-value", "cycle is a list of profile ids");
    } else {
      keys.cycle.forEach((id, i) => {
        if (keys.cycle.indexOf(id) !== i) error(`keys.cycle[${i}]`, "bad-value", `${id} is listed twice`);
        else if (!has(id)) warning(`keys.cycle[${i}]`, "unknown-profile", `no profile named ${id}: the cycle skips it`);
      });
    }
  }
  if (keys.henkan_enters !== undefined) {
    if (typeof keys.henkan_enters !== "string" || !ID.test(keys.henkan_enters)) {
      error("keys.henkan_enters", "bad-value", "henkan_enters is last or a profile id");
    } else if (keys.henkan_enters !== "last" && !has(keys.henkan_enters)) {
      warning("keys.henkan_enters", "unknown-profile", `no profile named ${keys.henkan_enters}: henkan enters the first profile`);
    }
  }
  if (keys.caps_lock !== undefined && !["transient", "respect"].includes(keys.caps_lock)) {
    error("keys.caps_lock", "bad-value", "caps_lock is transient or respect");
  }
  if (keys.caps_rule !== undefined && !["allcaps", "upper_snake"].includes(keys.caps_rule)) {
    error("keys.caps_rule", "bad-value", "caps_rule is allcaps or upper_snake");
  }
  return problems;
}
