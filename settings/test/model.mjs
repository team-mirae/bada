// The page's model (settings/src/model.js), with no browser: node settings/test/model.mjs
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import * as M from "../src/model.js";

const root = fileURLToPath(new URL("../../", import.meta.url));
const example = () => M.read(readFileSync(root + "docs/schemas/examples/settings.json", "utf8"));
const cases = JSON.parse(readFileSync(root + "docs/cases/key-table.json", "utf8"));
const stock = readdirSync(root + "engine/layouts").filter(f => f.endsWith(".json")).sort().map(f => {
  const text = readFileSync(root + "engine/layouts/" + f, "utf8");
  return { id: JSON.parse(text).id, text };
});

let passed = 0, failed = 0;
function test(name, run) {
  try { run(); passed++; } catch (e) { failed++; console.log(`  FAIL ${name}\n      ${String(e.stack ?? e.message).split("\n").slice(0, 8).join("\n      ")}`); }
}

test("a new install's file, and a file read", () => {
  const fresh = M.fresh();
  assert.deepEqual(fresh, { format: "bada-settings/2", engine: { muhenkan: {}, henkan: { profiles: [] } }, keys: {}, harness: {} });
  assert.equal(M.print(fresh).endsWith("}\n"), true);
  assert.equal(M.read("{"), null);
  assert.equal(M.read("[]"), null);
  assert.deepEqual(M.read('{"format": "bada-settings/2"}'), fresh, "the parts the page changes are made present");
  const file = example();
  assert.deepEqual(M.profiles(file).map(p => p.id), ["main", "work", "ja", "sym"]);
  assert.deepEqual(M.read(M.print(file)), file, "printing and reading keeps everything");
});

test("the presets are the standard's", () => {
  for (const [name, table] of [["jis-mac", "jis-mac"], ["jis-pc", "jis-pc"], ["none", "no-language-keys"]]) {
    assert.deepEqual(M.preset(name), cases.tables[table].bindings, name);
  }
  assert.equal(M.platformPreset("mac"), "jis-mac");
  assert.equal(M.platformPreset("linux"), "jis-pc");
});

test("a chord is read, written and named", () => {
  assert.deepEqual(M.parseOn("ctrl+alt+shift+meta+Space"), { code: "Space", mods: ["ctrl", "alt", "shift", "meta"] });
  assert.deepEqual(M.parseOn("KanaMode+Lang1"), { pair: ["KanaMode", "Lang1"] });
  for (const bad of ["shift+ctrl+Lang1", "Lang1+Lang1", "CapsLock+Lang1", "space", "", null]) assert.equal(M.parseOn(bad), null, String(bad));
  // Every `on` of the standard's cases is one the page reads.
  for (const table of Object.values(cases.tables)) for (const b of table.bindings) assert.notEqual(M.parseOn(b.on), null, b.on);
  assert.equal(M.chordOf({ code: "Tab", shift: true, ctrl: false, alt: false, meta: false }), "shift+Tab");
  assert.equal(M.chordOf({ code: "Space", shift: true, ctrl: true, alt: true, meta: true }), "ctrl+alt+shift+meta+Space");
  assert.deepEqual(M.keyLabels("shift+Lang1", "mac"), ["⇧かな"]);
  assert.deepEqual(M.keyLabels("ctrl+shift+Convert", "windows"), ["Ctrl+Shift+変換"]);
  assert.deepEqual(M.keyLabels("KanaMode+Lang1", "mac"), ["カタカナ", "かな"]);
  assert.deepEqual(M.keyLabels("meta+KeyK", "linux"), ["Super+K"]);
});

test("whether the harness sees a key (§9.3)", () => {
  const mac = { available: ["Lang1", "Lang2", "Backquote"], chords: true };
  assert.equal(M.seen("Lang1", mac), true);
  assert.equal(M.seen("shift+Lang1", mac), true);
  assert.equal(M.seen("Convert", mac), false, "a language key that isn't among the available ones");
  assert.equal(M.seen("KanaMode+Lang1", mac), false, "a pair needs both");
  assert.equal(M.seen("shift+Tab", mac), true);
  assert.equal(M.seen("shift+Tab", { available: ["Lang1"], chords: false }), false, "another key needs chords");
  assert.equal(M.seen("Convert", null), true, "nothing published: nothing is marked");
});

test("actions are named, and a binding is said in words", () => {
  const file = example();
  const layouts = M.catalog(stock, []);
  assert.deepEqual(M.actionLabel("profile:work", file, layouts), { text: "Go to work", missing: false });
  assert.deepEqual(M.actionLabel("profile:gone", file, layouts), { text: "Go to “gone”", missing: true });
  assert.deepEqual(M.actionLabel("ja-kana.katakana", file, layouts), { text: "Japanese - Kana: Katakana", missing: false });
  assert.equal(M.actionLabel("ja-kana.nothing", file, layouts).missing, true);
  assert.equal(M.actionLabel("menu.next", file, layouts).text, "Next entry");
  const ids = M.allActions(file, layouts).map(a => a[0]);
  for (const id of ["mode.toggle", "profile:main", "profile:sym", "menu.prev", "ja-romaji.halfwidth-latin", "ko-2-set.hanja", "pass", "none"]) assert.ok(ids.includes(id), id);
  assert.equal(new Set(ids).size, ids.length, "no action is offered twice");
  // Every action of the standard's own tables has a name, but the ones that name what isn't there.
  for (const b of cases.tables.example.bindings) for (const a of b.do) assert.equal(M.actionLabel(a, file, layouts).missing, false, a);
  assert.equal(M.says({ on: "Lang1", do: ["menu.next", "profile:main"], when: "henkan" }, file, layouts),
    "Next entry while the menu has two rows or more; otherwise go to 한국어 unless already there; otherwise nothing.");
  assert.equal(M.says({ on: "shift+Tab", do: ["menu.prev", "mode.toggle"] }, file, layouts),
    "Previous entry while the menu has two rows or more; otherwise toggle muhenkan and henkan.");
  assert.equal(M.says({ on: "F13", do: ["pass", "fix"] }, file, layouts), "Give the key to the app.", "nothing after an action that always applies");
  assert.equal(M.says({ on: "F13", do: [] }, file, layouts), "No action listed: the key is swallowed.");
});

test("renaming and removing a profile reach the keys", () => {
  const file = example();
  file.keys.henkan_enters = "work";
  file.keys.bindings.push({ on: "F13", do: ["none"], when: "profile:work" });
  M.renameProfile(file, "work", "office");
  assert.deepEqual(M.profiles(file).map(p => p.id), ["main", "office", "ja", "sym"]);
  assert.deepEqual(file.keys.cycle, ["main", "office", "ja"]);
  assert.equal(file.keys.henkan_enters, "office");
  assert.ok(file.keys.bindings.some(b => b.do.includes("profile:office")) && !file.keys.bindings.some(b => b.do.includes("profile:work")));
  assert.equal(file.keys.bindings.at(-1).when, "profile:office");
  M.removeProfile(file, "office");
  assert.deepEqual(file.keys.cycle, ["main", "ja"]);
  assert.equal("henkan_enters" in file.keys, false);
  assert.ok(file.keys.bindings.some(b => b.do.includes("profile:office")), "a binding that named it stays, and is marked");
  assert.equal(M.actionLabel("profile:office", file).missing, true);
});

test("ids, and the first run", () => {
  assert.equal(M.freeId([], "main"), "main");
  assert.equal(M.freeId(["main", "main-2"], "main"), "main-3");
  assert.equal(M.slug("My Profile!"), "my-profile");
  assert.ok(M.isProfileId("ko-2") && !M.isProfileId("Ko") && !M.isProfileId("") && !M.isProfileId("a".repeat(33)));
  assert.ok(M.isLayoutId("ja-kana-mine") && !M.isLayoutId("kana") && !M.isLayoutId("japanese-kana") && !M.isLayoutId("ja-Kana"));
  const file = M.fresh();
  assert.equal(M.easy(file, "en-qwerty", "ja-romaji", "Japanese - Romaji"), "main");
  assert.deepEqual(file.engine, { muhenkan: { layout: "en-qwerty" }, henkan: { profiles: [{ id: "main", name: "Japanese - Romaji", layouts: ["ja-romaji"] }] } });
});

test("the layouts the page lists", () => {
  const mine = JSON.parse(stock.find(l => l.id === "ja-kana").text);
  const copy = { ...mine, id: "ja-kana-five", name: { en: "Kana, five forms" }, forms: [...mine.forms, { do: "script", to: "halfwidth-katakana" }, { do: "keys", width: "full" }, { do: "keys", width: "half" }] };
  const changed = { ...JSON.parse(stock.find(l => l.id === "ko-2-set").text), forms: [{ do: "jamo", keep: "initials" }] };
  const list = M.catalog(stock, [{ id: copy.id, text: JSON.stringify(copy) }, { id: changed.id, text: JSON.stringify(changed) }, { id: "broken", text: "{" }], "ko");
  assert.equal(list.length, stock.length + 1, "a file that can't be read is left out");
  const kana = list.find(l => l.id === "ja-kana");
  assert.deepEqual([kana.name, kana.stock, kana.modified, kana.custom, kana.read, kana.kind], ["일본어 - 가나", true, false, false, "kana", "henkan"]);
  assert.deepEqual(kana.forms, ["the reading in hiragana", "the reading in katakana"]);
  assert.deepEqual(kana.actions.map(a => a.id), ["hiragana", "katakana", "halfwidth-katakana", "fullwidth-latin", "halfwidth-latin"]);
  const five = list.at(-1);
  assert.deepEqual([five.id, five.custom, five.stock, five.name], ["ja-kana-five", true, false, "Kana, five forms"]);
  assert.deepEqual(five.forms.slice(2), ["the reading in half-width katakana", "the keys as typed, full-width", "the keys as typed, half-width"]);
  const two = list.find(l => l.id === "ko-2-set");
  assert.deepEqual([two.stock, two.modified, two.forms], [true, true, ["the initial of each syllable"]]);
  assert.equal(two.stockText, stock.find(l => l.id === "ko-2-set").text, "the stock file is kept, to go back to");
  const emojis = list.find(l => l.id === "uni-emojis");
  assert.deepEqual(emojis.options.map(o => [o.name, o.type, o.of.length]), [["skin_tones", "order", 6]]);
  // What a search looks through.
  const words = M.layoutWords(kana, ["日本語"]);
  for (const w of ["japanese", "일본어", "かな", "ja-kana", "composes kana", "日本語", "stock", "forms"]) assert.ok(words.includes(w.toLowerCase()), w);
  assert.ok(M.layoutWords(five).includes("yours") && M.layoutWords(two).includes("changed"));
});

test("the dictionary's file", () => {
  assert.deepEqual(M.readDictionary(null), { positive: [], negative: [], rest: {} });
  assert.equal(M.readDictionary("{"), null);
  assert.equal(M.readDictionary('{"format": "something-else"}'), null);
  const text = readFileSync(root + "docs/reference/dictionary-example.json", "utf8");
  const d = M.readDictionary(text);
  assert.deepEqual([d.positive.length, d.negative.length], [5, 3]);
  const again = JSON.parse(M.printDictionary(d)), was = JSON.parse(text);
  assert.deepEqual([again.format, again.version, again.positive, again.negative], ["bada-dictionary", 1, was.positive, was.negative]);
  for (const k of Object.keys(was)) assert.deepEqual(again[k], was[k], `${k} is kept`);
});

console.log(failed ? `FAIL model: ${passed} passed, ${failed} failed` : `ok   model: ${passed} passed, 0 failed`);
process.exit(failed ? 1 : 0);
