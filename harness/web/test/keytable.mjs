// Replays every case and every sequence of docs/cases/key-table.json (bada-key-cases/1) on the key
// table module, as the standard asks of each harness (§9.6).
//   node harness/web/test/keytable.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { KeyTable, PRESETS, after, check, decide, parseOn, tableOf } from "../src/keytable.js";

const root = fileURLToPath(new URL("../../../", import.meta.url));
const file = JSON.parse(readFileSync(root + "docs/cases/key-table.json", "utf8"));
if (file.format !== "bada-key-cases/1") throw new Error(`docs/cases/key-table.json is ${file.format}, not bada-key-cases/1`);

let passed = 0, failed = 0;
const fail = (name, why) => { failed++; console.log(`  FAIL ${name}\n      ${why}`); };

/** Where an outcome differs from what a case expects of it: only the fields the case names. */
function differs(expect, got) {
  for (const [field, value] of Object.entries(expect)) {
    if (field === "at") continue;
    if (got[field] !== value) return `${field}: ${JSON.stringify(got[field])}, expected ${JSON.stringify(value)}`;
  }
  return null;
}

const stateOf = s => ({ previous: null, staged: "", ...s });

for (const c of file.cases) {
  const table = tableOf(file.tables[c.table]);
  const got = decide(table, file.profiles[c.profiles], stateOf(c.state), c.press);
  const why = differs(c.expect, got);
  if (why) fail(c.name, `${why}\n      got: ${JSON.stringify({ ...got, press: undefined })}`);
  else passed++;
}
const cases = passed;
console.log(`${failed ? "FAIL" : "ok  "} cases: ${cases} of ${file.cases.length} passed`);

let sequences = 0;
for (const s of file.sequences ?? []) {
  const keys = new KeyTable(tableOf(file.tables[s.table]), file.profiles[s.profiles]);
  let state = stateOf(s.state);
  const got = [];
  s.keys.forEach((key, at) => {
    const [way, code] = key.split(":");
    // Each outcome is carried out before the next is decided: the state moves on.
    const emit = outcome => { got.push({ at, ...outcome }); state = after(state, outcome); };
    if (way === "down") keys.down({ code, ctrl: false, alt: false, shift: false, meta: false }, () => state, emit);
    else keys.up(code, () => state, emit);
  });
  let why = got.length === s.expect.length ? null : `${got.length} things happened, expected ${s.expect.length}`;
  for (let i = 0; !why && i < got.length; i++) {
    if (got[i].at !== s.expect[i].at) why = `[${i}] at: ${got[i].at}, expected ${s.expect[i].at}`;
    else if (differs(s.expect[i], got[i])) why = `[${i}] ${differs(s.expect[i], got[i])}`;
  }
  if (why) fail(s.name, `${why}\n      got: ${JSON.stringify(got.map(g => ({ ...g, press: undefined })))}`);
  else { passed++; sequences++; }
}
console.log(`${sequences === (file.sequences ?? []).length ? "ok  " : "FAIL"} sequences: ${sequences} of ${(file.sequences ?? []).length} passed`);

// The presets of §9.5 are the cases' own tables: the module's copies must be theirs.
const same = (name, a, b) => {
  if (JSON.stringify(a) === JSON.stringify(b)) passed++;
  else fail(name, `${JSON.stringify(a)}\n      is not ${JSON.stringify(b)}`);
};
same("preset jis-mac is the cases' table", PRESETS["jis-mac"], file.tables["jis-mac"].bindings);
same("preset jis-pc is the cases' table", PRESETS["jis-pc"], file.tables["jis-pc"].bindings);
same("preset none is the cases' table", PRESETS.none, file.tables["no-language-keys"].bindings);
same("no keys.bindings: the keyboard's preset", tableOf({}, "jis-pc").bindings.map(b => b.on), PRESETS["jis-pc"].map(b => b.on));
same("an empty keys.bindings is used as it is", tableOf({ bindings: [] }, "jis-pc").bindings, []);
same("the other fields' defaults", (({ cycle, henkan_enters, caps_lock, caps_rule }) => ({ cycle, henkan_enters, caps_lock, caps_rule }))(tableOf({})),
  { cycle: [], henkan_enters: "last", caps_lock: "respect", caps_rule: "allcaps" });

// What the cases don't say, and this module decides (README.md lists them).
const four = file.profiles.four;
const example = tableOf(file.tables.example);
const muhenkan = stateOf({ mode: "muhenkan", profile: null, menu_rows: 0, caps_lock: false });
same("a pair is parsed in either order", parseOn("Lang1+KanaMode"), { pair: ["Lang1", "KanaMode"] });
same("a pair of the same key is no pair", parseOn("Lang1+Lang1"), null);
same("Caps Lock is in no pair", parseOn("CapsLock+Lang1"), null);
{
  // A bound modifier runs when it is pressed and released alone, and not when another key came between.
  const table = tableOf({ bindings: [{ on: "MetaRight", do: ["profile.first"] }, { on: "MetaLeft", do: ["mode:muhenkan"] }] });
  const run = sequence => {
    const keys = new KeyTable(table, four);
    let state = muhenkan;
    const got = [];
    for (const [way, code, mods = {}] of sequence) {
      const emit = o => { got.push(o.then); state = after(state, o); };
      if (way === "down") keys.down({ code, ctrl: false, alt: false, shift: false, meta: false, ...mods }, () => state, emit);
      else keys.up(code, () => state, emit);
    }
    return { got, state };
  };
  const tap = run([["down", "MetaRight"], ["up", "MetaRight"]]);
  same("a bound modifier alone runs on release", [tap.got, tap.state.mode, tap.state.profile], [["select"], "henkan", "main"]);
  same("a modifier with another key is a chord, not its own binding",
    run([["down", "MetaRight"], ["down", "KeyC", { meta: true }], ["up", "KeyC"], ["up", "MetaRight"]]).got, ["key"]);
  same("an unbound modifier is nothing", run([["down", "ShiftLeft"], ["up", "ShiftLeft"]]).got, []);
}
{
  // A held key: what isn't the table's repeats, a bound key runs once.
  const keys = new KeyTable(example, four);
  let state = stateOf({ mode: "henkan", profile: "main", menu_rows: 3, staged: "한", caps_lock: false });
  const got = [];
  const emit = o => { got.push(o.then); state = after(state, o); };
  const press = (code, repeat, all = false) => keys.down({ code, ctrl: all, alt: all, shift: all, meta: all, repeat }, () => state, emit);
  press("KeyA", false); press("KeyA", true);
  press("Space", false, true); press("Space", true, true);
  press("F13", false); press("F13", true);
  press("Convert", false); press("Convert", true);
  same("a held key repeats what isn't the table's; a binding runs once, but for pass",
    got, ["key", "key", "select", "swallow", "pass", "pass", "swallow"]);
}
same("profile.next skips a profile of the cycle that isn't there",
  decide(tableOf({ bindings: [{ on: "F13", do: ["profile.next"] }], cycle: ["main", "gone", "ja"] }), four,
    stateOf({ mode: "henkan", profile: "main", menu_rows: 0, caps_lock: false }), "F13").profile, "ja");
same("check: a table with nothing wrong", check(file.tables["jis-mac"], file.profiles.one), []);
same("check: the example names profiles the list has", check(file.tables.example, four, [{ id: "ja-kana", actions: [{ id: "katakana" }] }]), []);
same("check: what is wrong is named",
  check({ bindings: [{ on: "shift+ctrl+Lang1", do: ["fix"] }, { on: "Lang1", do: ["jump"], when: "sometimes" }], caps_lock: "toggle", extra: 1 })
    .map(p => `${p.level} ${p.path} ${p.code}`),
  ["error keys.extra unknown-field", "error keys.bindings[0].on bad-value", "error keys.bindings[1].do[0] unknown-action",
    "error keys.bindings[1].when bad-value", "error keys.caps_lock bad-value"]);
same("check: a profile that isn't there is a warning",
  check(file.tables.odd, four).map(p => `${p.level} ${p.path} ${p.code}`),
  ["warning keys.bindings[2].do[0] unknown-profile", "warning keys.cycle[0] unknown-profile"]);

console.log(`${failed ? "FAIL" : "ok  "} key table: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
