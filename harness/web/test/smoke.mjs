// Types on the built wasm through the code the page uses (src/harness.js): the key table, the
// engine, the reply applied to the text field model, the settings from storage. No browser: a
// press is `synthetic`, so the model does what the app would with a key passed on.
//   node harness/web/test/smoke.mjs      (after node harness/web/build.mjs)
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { Engine, HARNESS } from "../src/engine.js";
import { Harness } from "../src/harness.js";
import { PRESETS } from "../src/keytable.js";
import { webHost } from "../src/settings-host.js";
import * as S from "../src/settings.js";

const here = fileURLToPath(new URL("..", import.meta.url));
const root = fileURLToPath(new URL("../../../", import.meta.url));
const wasm = await WebAssembly.compile(readFileSync(here + "wasm/target/wasm32-unknown-unknown/release/bada_web.wasm"));
const version = readFileSync(root + "VERSION", "utf8").trim();

/** Local storage, as the page has it. */
class Memory {
  constructor(kept = {}) { this.map = new Map(Object.entries(kept)); }
  getItem(k) { return this.map.get(k) ?? null; }
  setItem(k, v) { this.map.set(k, String(v)); }
  removeItem(k) { this.map.delete(k); }
}

/** A page: an engine, a harness on `storage`, and what the page would have been told. */
async function page(storage = new Memory(), platform = "mac") {
  const engine = await Engine.load(wasm, { init: { harness: { ...HARNESS, version } } });
  const sent = [];
  const send = engine.send.bind(engine);
  engine.send = (field, event) => { sent.push(event); return send(field, event); };
  const h = new Harness({ engine, storage, platform });
  const told = { onboarding: 0, warnings: [], sent };
  h.ononboarding = () => told.onboarding++;
  h.onwarn = text => told.warnings.push(text);
  return { h, storage, told };
}

const LETTERS = Object.fromEntries([..."abcdefghijklmnopqrstuvwxyz"].map(c => [c, "Key" + c.toUpperCase()]));
/** A press and its release. `key`: a code, with `shift+`, `ctrl+`, `alt+`, `meta+` before it. */
function tap(h, key, options = { synthetic: true }) {
  const parts = key.split("+");
  const code = parts.pop();
  const press = { code, ctrl: parts.includes("ctrl"), alt: parts.includes("alt"), shift: parts.includes("shift"), meta: parts.includes("meta"), repeat: false };
  const dealt = h.press(press, options);
  h.release(code);
  return dealt;
}
/** Types lower-case letters and spaces. */
const type = (h, text) => { for (const c of text) tap(h, c === " " ? "Space" : LETTERS[c]); };

const two = (keys = {}) => S.print({
  format: S.FORMAT,
  engine: { muhenkan: { layout: "en-qwerty" }, henkan: { profiles: [
    { id: "main", name: "한국어", layouts: ["ko-2-set"] },
    { id: "ja", name: "日本語", layouts: ["ja-romaji"] },
    { id: "work", name: "work", layouts: ["ko-2-set", "en-qwerty"] },
  ] } },
  keys,
  harness: {},
});

let passed = 0, failed = 0;
async function test(name, run) {
  try { await run(); passed++; } catch (e) { failed++; console.log(`  FAIL ${name}\n      ${String(e.stack ?? e.message).split("\n").slice(0, 12).join("\n      ")}`); }
}

await test("a new install: no layout, no profile; a key that would enter henkan opens the onboarding", async () => {
  const { h, told, storage } = await page();
  assert.deepEqual([h.mode, h.profile, h.profiles, h.effective.muhenkan.layout ?? null], ["muhenkan", null, [], null]);
  assert.equal(told.sent.find(e => e.type === "select")?.mode, "muhenkan", "select follows bada_new");
  tap(h, "KeyA");
  assert.equal(h.field.text, "", "with no muhenkan layout the engine has no say, and here there is no OS to type");
  assert.equal(tap(h, "KeyA", { osKey: "a" }), false, "in a browser the key is the browser's");
  tap(h, "Lang1");
  assert.deepEqual([told.onboarding, h.mode], [1, "muhenkan"]);
  assert.equal(storage.getItem(S.STORE.settings), null, "nothing is written until something is chosen");
  // Easy mode: the muhenkan layout and one henkan layout.
  const result = h.easy("en-qwerty", "ko-2-set");
  assert.deepEqual([result.ok, result.problems], [true, []]);
  assert.deepEqual(JSON.parse(storage.getItem(S.STORE.settings)).engine.henkan.profiles, [{ id: "main", name: "Korean - 2-set", layouts: ["ko-2-set"] }]);
  tap(h, "Lang1");
  assert.deepEqual([told.onboarding, h.mode, h.profile], [1, "henkan", "main"], "the key that was pressed works from then on");
});

await test("QWERTY in muhenkan: the engine says what each key types", async () => {
  const { h } = await page(new Memory({ [S.STORE.settings]: two() }));
  type(h, "hi");
  tap(h, "shift+KeyA"); tap(h, "IntlRo"); tap(h, "shift+IntlRo"); tap(h, "IntlYen"); tap(h, "shift+Digit2");
  assert.equal(h.field.text, "hiA\\_¥\"");
  tap(h, "shift+Digit0");
  assert.equal(h.field.text, "hiA\\_¥\"", "the JIS table's one empty slot types nothing");
  tap(h, "Space"); tap(h, "Enter"); type(h, "ok"); tap(h, "Backspace");
  assert.equal(h.field.text, "hiA\\_¥\" \no");
  // The OS's layout is consulted only to see whether it would type the same text (§6.4).
  assert.equal(tap(h, "KeyQ", { osKey: "q" }), false, "the OS types q too: the browser has the key");
  assert.equal(h.field.text, "hiA\\_¥\" \no");
  assert.equal(tap(h, "KeyQ", { osKey: "a" }), true, "an AZERTY OS would type a: the page types q");
  assert.equal(h.field.text, "hiA\\_¥\" \noq");
  assert.equal(tap(h, "meta+KeyC", {}), false, "a shortcut is the browser's");
  assert.equal(tap(h, "Backspace", {}), false, "so is a key the engine has no say on");
});

await test("a mode switch through the key table, Korean 2-set, and Space commits", async () => {
  const { h, told } = await page(new Memory({ [S.STORE.settings]: two() }));
  assert.equal(h.table.preset, "jis-mac", "no keys.bindings: the keyboard's preset");
  tap(h, "Lang1");
  assert.deepEqual([h.mode, h.profile], ["henkan", "main"]);
  type(h, "gks");
  assert.deepEqual([h.field.text, h.field.markedText, h.reply.staged], ["한", "한", "한"]);
  assert.deepEqual(h.field.segments, [{ start: 0, len: 1, kind: "live" }]);
  assert.deepEqual([h.published().mode, h.published().profile, h.published().staged, h.published().preedit.text], ["henkan", "main", "한", "한"]);
  tap(h, "Space");
  assert.deepEqual([h.field.text, h.field.marked, h.reply.staged], ["한 ", null, ""], "the commit, then the app's Space");
  type(h, "rmf");
  tap(h, "Lang2");
  assert.deepEqual([h.mode, h.profile, h.field.text, h.field.marked], ["muhenkan", "main", "한 글", null], "leaving henkan commits what is staged");
  type(h, "a");
  assert.equal(h.field.text, "한 글a");
  // A language key never reaches the engine as a key (§4).
  assert.deepEqual(told.sent.filter(e => e.type === "key" && /Lang|CapsLock|Convert|KanaMode|Backquote/.test(e.key)), []);
});

await test("romaji, a profile switch by a chord and by a pair, and the profile before", async () => {
  const keys = { bindings: [...PRESETS["jis-mac"],
    { on: "ctrl+alt+shift+meta+Space", do: ["profile.next"] },
    { on: "KanaMode+Lang1", do: ["profile:work"] },
    { on: "F13", do: ["profile.last"] }], cycle: ["main", "ja"] };
  const { h } = await page(new Memory({ [S.STORE.settings]: two(keys) }));
  assert.equal(h.table.preset, null, "the settings' own bindings are used as they are");
  tap(h, "Lang1");
  tap(h, "ctrl+alt+shift+meta+Space");
  assert.deepEqual([h.mode, h.profile, h.previous], ["henkan", "ja", "main"]);
  type(h, "ka");
  assert.deepEqual([h.field.text, h.field.markedText], ["か", "か"]);
  tap(h, "Enter");
  assert.equal(h.field.text, "か\n", "Enter commits, then the app has it");
  type(h, "nihon");
  tap(h, "ctrl+alt+shift+meta+Space");
  assert.deepEqual([h.profile, h.field.text, h.field.marked], ["main", "か\nにほん", null], "a switch of profile commits what is staged");
  // A pair: both keys down, both up. It runs when the second is released.
  h.press({ code: "Lang1" }, { synthetic: true });
  h.press({ code: "KanaMode" }, { synthetic: true });
  h.release("Lang1");
  assert.equal(h.profile, "main");
  h.release("KanaMode");
  assert.deepEqual([h.profile, h.previous], ["work", "main"]);
  // Alone, a key of that pair runs its own binding when it is released.
  type(h, "gk");
  assert.deepEqual(h.field.menu.rows.map(r => [r.text, r.layout]), [["하", "ko-2-set"], ["gk", "en-qwerty"]]);
  h.press({ code: "Lang1" }, { synthetic: true });
  assert.equal(h.field.menu.selected, 0);
  h.release("Lang1");
  assert.deepEqual([h.field.menu.selected, h.field.markedText], [1, "gk"], "かな in henkan is the next row");
  assert.equal(h.field.segments[0].kind, "picked");
  tap(h, "F13");
  assert.deepEqual([h.profile, h.previous, h.field.text], ["main", "work", "か\nにほんgk"]);
});

// かな on a Mac and 変換 on a PC (§9.5): the next entry in every profile while the menu has two rows
// or more, and otherwise the key to their profile. `ko` and `ja` have two layouts, so what is typed
// has two rows; `han` has one layout, so there is never more than one candidate.
const three = keys => S.print({
  format: S.FORMAT,
  engine: { muhenkan: { layout: "en-qwerty" }, henkan: { profiles: [
    { id: "ko", name: "한국어", layouts: ["ko-2-set", "en-qwerty"] },
    { id: "ja", name: "日本語", layouts: ["ja-kana", "en-qwerty"] },
    { id: "han", name: "한글", layouts: ["ko-2-set"] },
  ] } },
  keys,
  harness: {},
});
const seen = h => [h.mode, h.profile, h.field.menu.rows.map(r => r.text), h.field.menu.selected, h.field.markedText];

for (const [platform, main, leave, own] of [["mac", "Lang1", "Lang2", "Convert"], ["pc", "Convert", "NonConvert", "Lang1"]]) {
  // A table of the user's own: every profile has its key, and the keyboard's main key is the first one's.
  const keys = { bindings: [
    { on: leave, do: ["mode:muhenkan"] },
    { on: main, do: ["profile:ko"] },
    { on: main, do: ["menu.next", "profile:ko"], when: "henkan" },
    { on: "shift+" + main, do: ["menu.prev"], when: "henkan" },
    { on: "KanaMode", do: ["profile:ja", "menu.next"] },
    { on: own, do: ["profile:han", "menu.next"] },
  ] };

  await test(`${main} steps through the entries of another profile, and ⇧${main} back (${platform}, the settings' own keys)`, async () => {
    const { h, told } = await page(new Memory({ [S.STORE.settings]: three(keys) }), platform);
    tap(h, "KanaMode");
    assert.deepEqual([h.mode, h.profile], ["henkan", "ja"]);
    type(h, "t");
    assert.deepEqual(seen(h), ["henkan", "ja", ["か", "t"], 0, "か"]);
    tap(h, main);
    assert.deepEqual(seen(h), ["henkan", "ja", ["か", "t"], 1, "t"], "two rows: the next entry, and the profile stays");
    tap(h, main);
    assert.deepEqual(seen(h), ["henkan", "ja", ["か", "t"], 1, "t"], "at the last row it still isn't a profile's key: the menu doesn't wrap");
    tap(h, "shift+" + main);
    assert.deepEqual(seen(h), ["henkan", "ja", ["か", "t"], 0, "か"], "with ⇧, the entry before");
    assert.deepEqual(told.sent.filter(e => e.type === "action").map(e => e.name), ["menu.next", "menu.next", "menu.prev"]);
    assert.deepEqual(told.sent.filter(e => e.type === "select").map(e => e.profile), [null, "ja"], "no select was sent for them");
    // The profile's own key steps too.
    tap(h, "KanaMode");
    assert.deepEqual(seen(h), ["henkan", "ja", ["か", "t"], 1, "t"]);
  });

  await test(`${main} with nothing to step through goes to its profile (${platform}, the settings' own keys)`, async () => {
    const { h, told } = await page(new Memory({ [S.STORE.settings]: three(keys) }), platform);
    // Nothing staged.
    tap(h, "KanaMode");
    tap(h, main);
    assert.deepEqual(seen(h), ["henkan", "ko", [], null, ""], "no menu: to the first profile");
    // One candidate: a profile with one layout has no second row.
    tap(h, own);
    type(h, "gks");
    assert.deepEqual([...seen(h), h.reply.staged], ["henkan", "han", [], null, "한", "한"]);
    tap(h, "shift+" + main);
    assert.deepEqual([...seen(h), h.reply.staged], ["henkan", "han", [], null, "한", "한"], "⇧ with one candidate does nothing: it is no profile's key");
    tap(h, main);
    assert.deepEqual([...seen(h), h.field.text], ["henkan", "ko", [], null, "", "한"], "one candidate isn't stepped through: the profile changes, and what was staged is committed");
    assert.equal(told.sent.some(e => e.type === "action"), false, "no menu.next was sent");
    // After committing the menu is gone, and the key is the profile's again.
    tap(h, "KanaMode");
    type(h, "t");
    tap(h, main);
    tap(h, "Enter");
    assert.deepEqual([...seen(h), h.field.text], ["henkan", "ja", [], null, "", "한t\n"]);
    tap(h, main);
    assert.deepEqual([h.mode, h.profile], ["henkan", "ko"]);
    // In its own profile: the next entry, or nothing.
    type(h, "gks");
    tap(h, main);
    assert.deepEqual(seen(h), ["henkan", "ko", ["한", "gks"], 1, "gks"]);
    tap(h, "Enter");
    tap(h, "Digit1");
    tap(h, main);
    assert.deepEqual([...seen(h), h.reply.staged], ["henkan", "ko", [], null, "1", "1"], "one candidate in its own profile: nothing happens");
    // From muhenkan it enters its profile, whichever was last used.
    tap(h, "KanaMode");
    tap(h, leave);
    assert.deepEqual([h.mode, h.profile], ["muhenkan", "ja"]);
    tap(h, main);
    assert.deepEqual([h.mode, h.profile], ["henkan", "ko"]);
  });

  await test(`⇧${main} goes down into a layout's forms, and ⇧半角/全角 makes the kana half-width (${platform})`, async () => {
    const file = JSON.parse(three(keys));
    file.keys.bindings.push({ on: "shift+Backquote", do: ["width.half"] }, { on: "Backquote", do: ["width.full"] });
    file.engine.henkan.profiles[1].script_forms = true;
    const { h } = await page(new Memory({ [S.STORE.settings]: S.print(file) }), platform);
    const entries = () => h.field.menu.rows.map(r => [r.entry, r.text]);
    tap(h, "KanaMode");
    type(h, "tt");
    assert.deepEqual(entries(), [[-2, "カカ"], [-1, "かか"], [0, "かか"], [1, "tt"]], "the forms of ja-kana's file: hiragana, katakana");
    tap(h, "shift+" + main);
    assert.deepEqual([h.field.menu.selected, h.field.markedText, h.profile], [1, "かか", "ja"]);
    tap(h, "shift+" + main);
    assert.deepEqual([h.field.menu.selected, h.field.markedText, h.profile], [0, "カカ", "ja"]);
    tap(h, "shift+" + main);
    assert.deepEqual([h.field.menu.selected, h.field.markedText, h.profile], [0, "カカ", "ja"], "the lowest form: no wrap");
    // Katakana that shows, half-width; and back.
    tap(h, "shift+Backquote");
    assert.deepEqual([h.field.markedText, h.reply.staged, h.field.menu.rows], ["ｶｶ", "ｶｶ", []]);
    tap(h, "Backquote");
    assert.equal(h.field.markedText, "カカ");
    tap(h, "Enter");
    // Hiragana that shows, half-width.
    type(h, "t");
    tap(h, "shift+Backquote");
    assert.equal(h.field.markedText, "ｶ");
    tap(h, "Enter");
    assert.equal(h.field.text, "カカ\nｶ\n");
    // Kana already in the text, selected: the engine asks for the selection.
    h.field.text = "がっこう";
    h.field.start = 0;
    tap(h, "shift+Backquote");
    assert.equal(h.field.text, "ｶﾞｯｺｳ");
    // With the forms there are always rows to step through while something is staged.
    type(h, "t");
    tap(h, main);
    assert.deepEqual([h.profile, h.field.menu.selected], ["ja", 3]);
  });

  await test(`${main} in the keyboard's preset: the next entry in any profile, or else the first profile (${platform})`, async () => {
    const { h } = await page(new Memory({ [S.STORE.settings]: three({}) }), platform);
    assert.equal(h.table.preset, platform === "mac" ? "jis-mac" : "jis-pc");
    h.enter("henkan", "ja");
    type(h, "t");
    tap(h, main);
    assert.deepEqual(seen(h), ["henkan", "ja", ["か", "t"], 1, "t"]);
    tap(h, "shift+" + main);
    assert.deepEqual(seen(h), ["henkan", "ja", ["か", "t"], 0, "か"]);
    tap(h, "Enter");
    tap(h, main);
    assert.deepEqual([h.mode, h.profile], ["henkan", "ko"], "no menu: the first profile");
    h.enter("henkan", "han");
    type(h, "gks");
    tap(h, main);
    assert.deepEqual([h.mode, h.profile, h.field.text, h.field.marked], ["henkan", "ko", "か\n한", null], "one candidate: the first profile");
  });
}

await test("⇧Tab is the toggle while no menu is open, and the engine's while one is", async () => {
  const { h, told } = await page(new Memory({ [S.STORE.settings]: two() }));
  const selects = () => told.sent.filter(e => e.type === "select").length;
  assert.deepEqual([h.mode, h.profile], ["muhenkan", null]);
  assert.equal(tap(h, "shift+Tab", {}), true, "the browser doesn't get it: the focus stays in the field");
  assert.deepEqual([h.mode, h.profile], ["henkan", "main"], "from muhenkan, into henkan_enters' profile");
  tap(h, "shift+Tab");
  assert.deepEqual([h.mode, h.profile], ["muhenkan", "main"], "and back");
  // One candidate is no menu: the toggle, and what was staged is committed by the select.
  tap(h, "shift+Tab");
  type(h, "gks");
  assert.deepEqual([h.field.menu.rows, h.reply.staged], [[], "한"]);
  tap(h, "shift+Tab");
  assert.deepEqual([h.mode, h.field.text, h.field.marked], ["muhenkan", "한", null]);
  // A menu open: ⇧Tab is the engine's, the entry before, and the mode stays.
  h.enter("henkan", "work");
  type(h, "gk");
  tap(h, "Tab");
  assert.deepEqual([h.field.menu.rows.length, h.field.menu.selected], [2, 1]);
  const before = selects();
  tap(h, "shift+Tab");
  assert.deepEqual([h.mode, h.profile, h.field.menu.selected, h.field.markedText, selects()], ["henkan", "work", 0, "하", before]);
  assert.deepEqual(told.sent.at(-1), { type: "key", key: "Tab", shift: true, ctrl: false, alt: false, meta: false });
  tap(h, "Enter");
  // It enters the profile last used; held, it toggles once; with another modifier it isn't the toggle.
  tap(h, "shift+Tab");
  assert.deepEqual([h.mode, h.profile], ["muhenkan", "work"]);
  h.press({ code: "Tab", shift: true, ctrl: false, alt: false, meta: false, repeat: false }, { synthetic: true });
  h.press({ code: "Tab", shift: true, ctrl: false, alt: false, meta: false, repeat: true }, { synthetic: true });
  h.release("Tab");
  assert.deepEqual([h.mode, h.profile], ["henkan", "work"]);
  tap(h, "ctrl+shift+Tab");
  tap(h, "Tab");
  assert.equal(h.mode, "henkan", "Tab alone and ⌃⇧Tab are not it");
  // No profile at all: the onboarding, as the toggle has it.
  const fresh = await page();
  tap(fresh.h, "shift+Tab");
  assert.deepEqual([fresh.told.onboarding, fresh.h.mode], [1, "muhenkan"]);
  // A binding on shift+Tab in the settings comes first.
  const bound = await page(new Memory({ [S.STORE.settings]: two({ bindings: [{ on: "shift+Tab", do: ["pass"] }] }) }));
  assert.equal(tap(bound.h, "shift+Tab", {}), false, "pass: the browser has the key");
  assert.equal(bound.h.mode, "muhenkan");
  // transient: the lock is turned off on the way into henkan, as with any toggle.
  const lock = await page(new Memory({ [S.STORE.settings]: two({ caps_lock: "transient" }) }));
  tap(lock.h, "CapsLock");
  assert.equal(lock.h.caps, true);
  tap(lock.h, "shift+Tab");
  assert.deepEqual([lock.h.mode, lock.h.caps], ["henkan", false]);
});

await test("a click on a menu row picks and commits; the mode's own buttons select", async () => {
  const { h, told } = await page(new Memory({ [S.STORE.settings]: two() }));
  h.enter("henkan", "work");
  assert.deepEqual([h.mode, h.profile], ["henkan", "work"]);
  type(h, "gk");
  h.pick(1);
  assert.deepEqual([h.field.text, h.field.marked, h.field.menu.rows], ["gk", null, []]);
  assert.deepEqual(told.sent.slice(-2).map(e => e.type), ["pick", "flush"]);
  type(h, "gk");
  h.flush("click");
  assert.deepEqual([h.field.text, told.sent.at(-1)], ["gk하", { type: "flush", cause: "click" }]);
  h.enter("muhenkan");
  assert.deepEqual([h.mode, h.profile], ["muhenkan", "work"]);
  h.enter("henkan");
  assert.deepEqual([h.mode, h.profile], ["henkan", "work"], "henkan enters the profile last used");
});

await test("an ask is answered once with the selection; ⇧半角/全角 makes half-width", async () => {
  const { h, told } = await page(new Memory({ [S.STORE.settings]: two() }));
  type(h, "abc");
  h.field.start = 0;   // the user selects abc
  tap(h, "Backquote");
  assert.equal(h.field.text, "ａｂｃ");
  assert.deepEqual(told.sent.slice(-2), [{ type: "action", name: "width.full" }, { type: "text", selection: "abc" }]);
  h.field.start = 0;
  tap(h, "shift+Backquote");
  assert.equal(h.field.text, "abc");
});

await test("Caps Lock is the page's: capitals of what the engine types, and UPPER_SNAKE", async () => {
  const { h } = await page(new Memory({ [S.STORE.settings]: two({ caps_lock: "transient", caps_rule: "upper_snake" }) }));
  assert.equal(tap(h, "CapsLock", {}), false, "unbound, the key is the lock, and the OS has it");
  assert.equal(h.caps, true);
  type(h, "ab"); tap(h, "Digit1"); tap(h, "Space"); tap(h, "alt+Space");
  assert.equal(h.field.text, "AB1_ ");
  assert.equal(tap(h, "KeyC", { osKey: "c" }), true, "the OS would type c, and the lock wants C: the page types it");
  tap(h, "Lang1");
  assert.deepEqual([h.mode, h.caps], ["henkan", false], "transient: leaving muhenkan turns the lock off");
  tap(h, "CapsLock");
  assert.equal(h.caps, false, "and in henkan the key does nothing");
  assert.deepEqual(h.published().harness.caps_lock, { on: false, setting: "transient", rule: "upper_snake" });
  // Bound, Caps Lock is no lock: the preset of a keyboard with no language keys.
  const none = await page(new Memory({ [S.STORE.settings]: S.print(S.withOwn(JSON.parse(two()), "keyboard", "none")) }));
  tap(none.h, "CapsLock");
  assert.deepEqual([none.h.mode, none.h.profile, none.h.caps, none.h.table.preset], ["henkan", "main", false, "none"]);
});

await test("the preset follows the platform, then the language keys the page sees", async () => {
  const pc = await page(new Memory({ [S.STORE.settings]: two() }), "pc");
  assert.equal(pc.h.table.preset, "jis-pc");
  tap(pc.h, "Convert");
  assert.deepEqual([pc.h.mode, pc.h.profile], ["henkan", "main"]);
  tap(pc.h, "NonConvert");
  assert.equal(pc.h.mode, "muhenkan");
  tap(pc.h, "Lang1");   // this keyboard has かな after all
  assert.deepEqual([pc.h.table.preset, pc.h.mode, pc.storage.getItem(S.STORE.seen)], ["jis-mac", "henkan", "jis-mac"]);
  assert.ok(pc.h.published().harness.keys.available.includes("Lang1"));
  // The user's own choice stands, whatever is seen.
  assert.equal(pc.h.setOwn("keyboard", "jis-pc").ok, true);
  tap(pc.h, "Lang2");
  assert.deepEqual([pc.h.table.preset, pc.h.mode], ["jis-pc", "henkan"], "英数 has no binding in that preset");
  assert.equal(JSON.parse(pc.storage.getItem(S.STORE.settings)).harness[S.WEB].keyboard, "jis-pc");
});

await test("settings: nothing wrong is taken silently, and what is wrong changes nothing", async () => {
  const { h, storage } = await page(new Memory({ [S.STORE.settings]: two() }));
  const kept = storage.getItem(S.STORE.settings);
  const codes = text => { const r = h.loadSettings(text); return [r.ok, ...r.problems.map(p => `${p.level} ${p.path} ${p.code}`)]; };
  const file = change => { const f = JSON.parse(two()); change(f); return JSON.stringify(f); };
  assert.deepEqual(codes("{"), [false, "error  bad-json"]);
  assert.deepEqual(codes("[]"), [false, "error  bad-value"]);
  assert.deepEqual(codes(file(f => { f.format = "bada-settings/1"; })), [false, "error format bad-format"]);
  assert.deepEqual(codes(file(f => { f.extra = 1; })), [false, "error extra unknown-field"]);
  assert.deepEqual(codes(file(f => { f.engine.muhenkan.layout = "ko-2-set"; })), [false, "error engine.muhenkan.layout not-muhenkan"]);
  assert.deepEqual(codes(file(f => { f.engine.henkan.profiles[1].layouts = ["ja-nothing"]; })), [false, "error engine.henkan.profiles[1].layouts[0] unknown-layout"]);
  assert.deepEqual(codes(file(f => { f.keys = { bindings: [{ on: "Lang1+Lang1", do: ["fix"] }] }; })), [false, "error keys.bindings[0].on bad-value"]);
  assert.deepEqual(codes(file(f => { f.harness = { [S.WEB]: { keyboard: "dvorak" } }; })), [false, `error harness.${S.WEB}.keyboard bad-value`]);
  assert.deepEqual(codes(file(f => { f.engine.muhenkan.layout = "ko-2-set"; f.keys = { caps_lock: "toggle" }; })),
    [false, "error keys.caps_lock bad-value", "error engine.muhenkan.layout not-muhenkan"], "every part's problems show at once");
  assert.equal(storage.getItem(S.STORE.settings), kept, "what is kept is the last good file");
  assert.deepEqual(h.profiles.map(p => p.id), ["main", "ja", "work"]);
  assert.equal(h.published().harness.problems[0].file, "settings.json", "the problems are in the published state");
  // A dry check changes nothing either, right or wrong.
  const dry = h.loadSettings(file(f => { f.engine.henkan.profiles = []; }), false, true);
  assert.deepEqual([dry.ok, h.profiles.length, storage.getItem(S.STORE.settings) === kept], [true, 3, true]);
  // A warning is taken, and said.
  assert.deepEqual(codes(file(f => { f.keys = { bindings: [{ on: "Lang1", do: ["profile:gone", "profile.first"] }], cycle: ["gone"] }; })),
    [true, "warning keys.bindings[0].do[0] unknown-profile", "warning keys.cycle[0] unknown-profile"]);
  assert.notEqual(storage.getItem(S.STORE.settings), kept);
  tap(h, "Lang1");
  assert.deepEqual([h.mode, h.profile], ["henkan", "main"]);
  // The profile in use is removed: what is staged is committed, and the field is back in muhenkan.
  type(h, "gks");
  assert.deepEqual(codes(file(f => { f.engine.henkan.profiles.shift(); })), [true]);
  assert.deepEqual([h.mode, h.profile, h.field.text, h.field.marked], ["muhenkan", null, "한", null]);
});

await test("the page comes back as it was left; a file moves to another page", async () => {
  const first = await page(new Memory({ [S.STORE.settings]: two() }));
  first.h.enter("henkan", "ja");
  const again = await page(first.storage);
  assert.deepEqual([again.h.mode, again.h.profile], ["henkan", "ja"], "the profile is kept as its id");
  assert.deepEqual(again.told.sent.find(e => e.type === "select"), { type: "select", mode: "henkan", profile: "ja" });
  type(again.h, "ka");
  assert.equal(again.h.field.markedText, "か");
  // Export is the file as it is kept; import is the same door as the editor.
  const exported = again.h.settingsText;
  const other = await page(new Memory(), "pc");
  assert.equal(other.h.loadSettings(exported).ok, true);
  assert.deepEqual(other.h.file, again.h.file);
  assert.equal(other.h.settingsText, exported);
});

await test("settings in storage that can't be used change nothing, and are not replaced", async () => {
  const { h, storage, told } = await page(new Memory({ [S.STORE.settings]: '{"format": "bada-settings/2", "engine": ' }));
  assert.deepEqual([h.broken, h.profiles, h.problems.settings[0].code], [true, [], "bad-json"]);
  assert.equal(h.settingsText, '{"format": "bada-settings/2", "engine": ', "the editor shows what is kept");
  assert.equal(h.setOwn("keyboard", "pc").ok, false);
  assert.equal(h.easy("en-qwerty", "ko-2-set").ok, false);
  assert.equal(storage.getItem(S.STORE.settings), '{"format": "bada-settings/2", "engine": ');
  tap(h, "Lang1");
  assert.equal(told.onboarding, 1);
  assert.equal(h.loadSettings(two()).ok, true, "a good file from the editor replaces it");
  assert.equal(h.broken, false);
});

await test("a custom layout from storage, one that replaces a stock layout, and one that is wrong", async () => {
  const stock = JSON.parse(readFileSync(root + "engine/layouts/en-qwerty.json", "utf8"));
  const mine = { format: "bada-layout/3", id: "en-greek", name: { en: "English - with α" }, lang: "en", kind: "muhenkan", read: "direct", keys: { KeyA: { base: { t: "α", r: "l" } } } };
  const { h, storage } = await page(new Memory({ [S.STORE.settings]: two() }));
  const tried = h.loadLayout(JSON.stringify(mine), true);
  assert.deepEqual([tried.ok, tried.problems.map(p => `${p.level} ${p.code}`), storage.getItem(S.STORE.layouts)], [true, ["warning no-model"], null], "a dry run shows the warning and keeps nothing");
  assert.equal(h.loadLayout(JSON.stringify(mine)).ok, true);
  assert.deepEqual(h.layouts.find(l => l.id === "en-greek") && [h.layouts.find(l => l.id === "en-greek").stock, Object.keys(h.customLayouts)], [false, ["en-greek"]]);
  const file = JSON.parse(two());
  file.engine.muhenkan.layout = "en-greek";
  assert.equal(h.loadSettings(JSON.stringify(file)).ok, true);
  type(h, "ab");
  assert.equal(h.field.text, "αb");
  // The next page feeds it from storage before the settings that name it.
  const again = await page(storage);
  assert.deepEqual([again.h.problems.settings, again.h.effective.muhenkan.layout], [[], "en-greek"]);
  type(again.h, "a");
  assert.equal(again.h.field.text, "α");
  // A changed copy of a stock layout replaces it, with a warning, until it is removed.
  stock.keys.KeyB = { base: { t: "β", r: "l" } };
  const replaced = h.loadLayout(JSON.stringify(stock));
  assert.deepEqual([replaced.ok, replaced.problems.map(p => p.code)], [true, ["stock-modified"]]);
  assert.deepEqual([h.layouts.find(l => l.id === "en-qwerty").stock, h.layouts.find(l => l.id === "en-qwerty").modified], [true, true]);
  assert.equal(h.removeLayout("en-qwerty").ok, true);
  assert.equal(h.layouts.find(l => l.id === "en-qwerty").modified, false);
  // What is wrong isn't loaded, and isn't kept.
  const wrong = h.loadLayout(JSON.stringify({ ...mine, id: "en-wrong", keys: { NoSuchKey: { base: { t: "x", r: "l" } } } }));
  assert.equal(wrong.ok, false);
  assert.ok(wrong.problems.some(p => p.level === "error"), JSON.stringify(wrong.problems));
  assert.equal(h.loadLayout("{").problems[0].code, "bad-json");
  assert.deepEqual(Object.keys(h.customLayouts), ["en-greek"]);
});

await test("the harness is a host of the settings page: the folder is storage", async () => {
  const { h, storage } = await page();
  const stock = Object.fromEntries(["en-qwerty", "ko-2-set", "ja-kana"].map(id => [id, JSON.parse(readFileSync(root + `engine/layouts/${id}.json`, "utf8"))]));
  const host = webHost(() => h, () => stock);
  assert.deepEqual(await host.info(), { platform: "web", harness: S.WEB, version, folder: "this browser's local storage", embedded: true });
  assert.equal((await host.describe()).abi.major, 2);
  assert.deepEqual((await host.stock()).map(l => [l.id, JSON.parse(l.text).id]), [["en-qwerty", "en-qwerty"], ["ko-2-set", "ko-2-set"], ["ja-kana", "ja-kana"]]);
  assert.deepEqual(await host.files(), { settings: null, dictionary: null, layouts: [], history: [] }, "a new install: nothing kept");
  // A check changes nothing; a write is taken, kept and in effect.
  const checked = await host.check({ settings: two() });
  assert.deepEqual([checked.ok, checked.problems, checked.layouts.length, h.profiles.length, storage.getItem(S.STORE.settings)], [true, [], 11, 0, null]);
  const written = await host.write("settings.json", two());
  assert.deepEqual([written.ok, written.files.settings.text, h.profiles.map(p => p.id)], [true, two(), ["main", "ja", "work"]]);
  // What is wrong is refused, with where: nothing changes.
  const wrong = JSON.parse(two());
  wrong.keys = { bindings: [{ on: "Lang1", do: ["jump"] }] };
  const refused = await host.write("settings.json", JSON.stringify(wrong));
  assert.deepEqual([refused.ok, refused.problems.map(p => `${p.level} ${p.path} ${p.code}`), refused.files.settings.text], [false, ["error keys.bindings[0].do[0] unknown-action"], two()]);
  assert.equal((await host.check({ settings: "{" })).problems[0].code, "bad-json");
  // A layout of the user's: its file is named by its id.
  const mine = { ...stock["ja-kana"], id: "ja-kana-five", name: { en: "Kana, five forms" } };
  assert.equal((await host.write("layouts/ja-other.json", JSON.stringify(mine))).ok, false, "the name isn't the file's id");
  const made = await host.write("layouts/ja-kana-five.json", JSON.stringify(mine));
  assert.deepEqual([made.ok, made.files.layouts.map(l => l.id), made.layouts.length], [true, ["ja-kana-five"], 12]);
  assert.equal((await host.check({ layout: JSON.stringify({ ...mine, format: "bada-layout/2" }) })).ok, false);
  const gone = await host.remove("layouts/ja-kana-five.json");
  assert.deepEqual([gone.ok, gone.files.layouts, h.layouts.length], [true, [], 11]);
  // The dictionary, and what the page can't do here.
  const dictionary = '{"format": "bada-dictionary", "version": 1, "positive": [{"word": "데이터센터"}], "negative": []}';
  assert.deepEqual([(await host.write("dictionary.json", dictionary)).files.dictionary.text, h.dictionary], [dictionary, { positive: 1, negative: 0 }]);
  assert.equal((await host.remove("dictionary.json")).files.dictionary, null);
  assert.equal((await host.write("../settings.json", "{}")).ok, false);
  assert.equal((await host.undo("settings-20261126-000000.json")).ok, false);
  assert.equal((await host.state()).harness.keys.chords, true, "the published state, for the keys it sees");
});

await test("the user dictionary from storage", async () => {
  const example = readFileSync(root + "docs/reference/dictionary-example.json", "utf8");
  const { h, storage } = await page();
  assert.deepEqual([h.loadDictionary(example).ok, h.dictionary], [true, { positive: 5, negative: 3 }]);
  assert.equal(storage.getItem(S.STORE.dictionary), example);
  const again = await page(storage);
  assert.deepEqual(again.h.dictionary, { positive: 5, negative: 3 });
  const wrong = h.loadDictionary('{"format": "something-else"}');
  assert.deepEqual([wrong.ok, wrong.problems[0].level, h.dictionary, storage.getItem(S.STORE.dictionary)], [false, "error", { positive: 5, negative: 3 }, example]);
  assert.deepEqual([h.loadDictionary("").ok, h.dictionary, storage.getItem(S.STORE.dictionary)], [true, { positive: 0, negative: 0 }, null]);
});

await test("the published state is bada-compatible-state/2", async () => {
  const { h } = await page(new Memory({ [S.STORE.settings]: two() }));
  h.enter("henkan", "work");
  type(h, "gk");
  const state = h.published();
  assert.deepEqual(Object.keys(state), ["format", "mode", "profile", "staged", "preedit", "menu", "ask", "dir", "harness"]);
  assert.deepEqual([state.format, state.mode, state.profile, state.staged, state.dir, state.ask], ["bada-compatible-state/2", "henkan", "work", "하", "ltr", null]);
  assert.deepEqual(state.preedit, { text: "하", caret: 1, segments: [{ len: 1, kind: "live" }] });
  assert.deepEqual([state.menu.rows.length, state.menu.selected, state.menu.strip], [2, 0, false]);
  assert.deepEqual([state.harness.id, state.harness.version, state.harness.secure, state.harness.keys.chords], [S.WEB, version, false, true]);
});

await test("a trap in the engine is a panic: the marked text and the menu go, and the engine starts again", async () => {
  const { h, told } = await page(new Memory({ [S.STORE.settings]: two() }));
  h.enter("henkan", "work");
  type(h, "gk");
  assert.deepEqual([h.field.markedText, h.field.menu.rows.length], ["하", 2]);
  // WebAssembly can't unwind: a panic in the engine traps. The next send does.
  const real = h.engine.x;
  h.engine.x = { ...real, memory: real.memory, bada_send() { throw new WebAssembly.RuntimeError("unreachable"); } };
  type(h, "s");
  assert.deepEqual([h.field.text, h.field.marked, h.field.menu.rows, h.reply.staged], ["", null, [], ""]);
  assert.deepEqual([h.engine.restarts, told.warnings.length], [1, 1]);
  // The new engine was given the settings, and the field its mode and profile.
  type(h, "gks");
  assert.deepEqual([h.mode, h.profile, h.field.markedText, h.field.menu.rows.map(r => r.text)], ["henkan", "work", "한", ["한", "gks"]]);
});

await test("an engine of another major version is refused; named buffers reach bada_data_new", async () => {
  const init = { harness: { ...HARNESS, version } };
  // An engine whose P_describe says 3.0.
  const memory = new WebAssembly.Memory({ initial: 1 });
  new Uint8Array(memory.buffer).set(new TextEncoder().encode('{"abi":{"name":"bada-compatible","major":3,"minor":0}}\0'), 8);
  const other = new Engine(null, init, []);
  other.x = { memory, bada_describe: () => 8 };
  assert.throws(() => other.start(), /bada-compatible 3\.0: the page needs bada-compatible 2/);
  // The place the data will come through (§11): a name the engine doesn't know is ignored.
  const engine = await Engine.load(wasm, { init, buffers: [{ name: "not-a-buffer", bytes: new Uint8Array([1, 2, 3]) }, { name: "also-not", bytes: new Uint8Array(70000) }] });
  assert.ok(engine.data !== 0 && engine.session !== 0);
  assert.deepEqual([engine.describe.abi.major, engine.describe.engine.id], [2, "com.teammirae.bada"]);
  engine.close();
});

console.log(`${failed ? "FAIL" : "ok  "} smoke: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
