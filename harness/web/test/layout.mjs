// The layout editor's model (src/layout-model.js) on the layout schema and the stock layouts, and
// its new layouts on the built wasm. No browser.
//   node harness/web/test/layout.mjs      (after node harness/web/build.mjs)
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { Engine, HARNESS } from "../src/engine.js";
import * as L from "../src/layout-model.js";

const here = fileURLToPath(new URL("..", import.meta.url));
const root = fileURLToPath(new URL("../../../", import.meta.url));
const schema = JSON.parse(readFileSync(root + "docs/schemas/layout.schema.json", "utf8"));
const stock = readdirSync(root + "engine/layouts").filter(f => f.endsWith(".json")).map(f => JSON.parse(readFileSync(root + "engine/layouts/" + f, "utf8")));
const wasm = await WebAssembly.compile(readFileSync(here + "wasm/target/wasm32-unknown-unknown/release/bada_web.wasm"));
const engine = await Engine.load(wasm, { init: { harness: { ...HARNESS, version: "0" } } });
const errors = doc => (engine.send(null, { type: "layout", value: doc, dry: true }).problems ?? []).filter(p => p.level === "error");

let passed = 0;
function test(name, run) {
  try { run(); passed++; console.log("ok   " + name); } catch (e) { console.log("FAIL " + name + "\n" + (e.stack ?? e)); process.exitCode = 1; }
}

test("every stock layout fits the schema", () => {
  assert.equal(stock.length, 11);
  for (const doc of stock) assert.deepEqual(L.validate(schema, doc), [], doc.id);
});

test("every place of every stock layout has a schema, and every field a description", () => {
  const bare = new Set();
  for (const doc of stock) {
    for (const place of L.places(doc)) {
      const path = L.parse(place), told = L.describe(schema, doc, place);
      assert.ok(told, `${doc.id}${place} has no schema`);
      // A field of the format is annotated. An entry of a map or a list is told by its parent.
      const parent = L.schemaAt(schema, doc, path.slice(0, -1));
      const form = parent.oneOf ? L.deref(schema, parent.oneOf[parent.variant]) : parent;
      if (form.properties && Object.hasOwn(form.properties, path.at(-1)) && !told.description) bare.add(place.replace(/\/\d+(?=\/|$)/g, "/0"));
      assert.equal(told.present, true);
      assert.deepEqual(told.value, L.get(doc, path));
    }
  }
  assert.deepEqual([...bare], []);
});

test("every field the schema has is described", () => {
  const bare = [];
  const walk = (node, at) => {
    if (node === null || typeof node !== "object") return;
    if (Array.isArray(node)) return node.forEach((n, i) => walk(n, `${at}/${i}`));
    for (const [name, prop] of Object.entries(node.properties ?? {})) {
      if (!L.deref(schema, prop).description) bare.push(`${at}/properties/${name}`);
    }
    // The parts under allOf only narrow fields that are described where they are declared.
    for (const [key, value] of Object.entries(node)) if (key !== "allOf") walk(value, `${at}/${key}`);
  };
  walk(schema, "");
  assert.deepEqual(bare, []);
});

test("what doesn't fit is told where it is", () => {
  const qwerty = () => L.copy(stock.find(l => l.id === "en-qwerty"));
  const at = (doc, path) => L.validate(schema, doc).filter(p => p.path === path).map(p => p.message);
  let doc = qwerty(); doc.id = "Qwerty";
  assert.equal(at(doc, "/id").length, 1);
  doc = qwerty(); doc.extra = 1;
  assert.deepEqual(at(doc, "/extra"), ["isn't a field of the format"]);
  doc = qwerty(); delete doc.lang;
  assert.deepEqual(at(doc, "/lang"), ["is missing"]);
  doc = qwerty(); doc.keys.KeyA = { base: { t: "ㅁ", r: "c" } };
  assert.equal(at(doc, "/keys/KeyA/base/r").length, 1, "a role of another way of reading");
  doc = qwerty(); doc.keys.Enter = { base: null };
  assert.equal(at(doc, "/keys/Enter").length, 1);
  doc = qwerty(); doc.hangul = { vowels: {}, finals: {}, double_tense: false, final_moves_on: true };
  assert.deepEqual(at(doc, "/hangul"), ["isn't part of this kind of layout"]);
  doc = qwerty(); doc.kind = "henkan";
  assert.equal(at(doc, "/kind").length, 1);
  doc = L.copy(stock.find(l => l.id === "ja-romaji")); doc.convert[1].to = "wide";
  assert.equal(at(doc, "/convert/1/to").length, 1, "the form in use is the one that is told");
  doc = L.copy(stock.find(l => l.id === "ja-romaji")); delete doc.table;
  assert.deepEqual(at(doc, "/table"), ["is missing"]);
});

test("pointers get, set and remove", () => {
  const doc = { a: { "b/c": [1, 2] } };
  assert.equal(L.pointer(["a", "b/c", 0]), "/a/b~1c/0");
  assert.deepEqual(L.parse("/a/b~1c/0"), ["a", "b/c", "0"]);
  assert.equal(L.get(doc, L.parse("/a/b~1c/1")), 2);
  L.set(doc, L.parse("/a/b~1c/2"), 3);
  L.set(doc, L.parse("/x/y"), true);
  L.set(doc, L.parse("/list/0"), "first");
  assert.deepEqual(doc, { a: { "b/c": [1, 2, 3] }, x: { y: true }, list: ["first"] });
  assert.equal(L.remove(doc, L.parse("/a/b~1c/0")), true);
  assert.equal(L.remove(doc, L.parse("/x/nothing")), false);
  assert.deepEqual(doc.a["b/c"], [2, 3]);
  assert.throws(() => L.set(doc, [], {}));
});

test("the schema of a place follows the form in use", () => {
  const romaji = stock.find(l => l.id === "ja-romaji"), roman = stock.find(l => l.id === "ko-bada-roman");
  assert.equal(L.schemaAt(schema, romaji, ["convert", "0"]).variant, 0);
  assert.equal(L.schemaAt(schema, romaji, ["convert", "1"]).variant, 2);
  assert.deepEqual(L.schemaAt(schema, romaji, ["convert", "1", "to"]).enum, ["half", "full"]);
  assert.equal(L.schemaAt(schema, romaji, ["table"]).variant, 0);
  assert.equal(L.schemaAt(schema, roman, ["table"]).variant, 1);
  assert.ok(L.schemaAt(schema, roman, ["table", "syllable", "onsets"]).description);
  assert.deepEqual(L.schemaAt(schema, romaji, ["keys", "KeyA", "base", "r"]).enum, ["l", "b", "x"], "the roles of its way of reading");
  assert.equal(L.schemaAt(schema, romaji, ["keys", "KeyA", "base"]).variant, null, "a slot left out");
  assert.equal(L.schemaAt(schema, romaji, ["nothing"]), null);
  const told = L.describe(schema, romaji, "/rank/known");
  assert.equal(told.limits, "number");
  assert.equal(told.required, false);
  assert.equal(L.describe(schema, romaji, "/id").required, true);
  assert.equal(L.describe(schema, romaji, "/convert/1/to").required, true);
});

test("a slot is the file's, else the JIS table's", () => {
  const ko = stock.find(l => l.id === "ko-2-set"), qwerty = stock.find(l => l.id === "en-qwerty");
  assert.deepEqual(L.slotOf(ko, "KeyR", "base"), { slot: { t: "ㄱ", r: "c" }, from: "file" });
  assert.deepEqual(L.slotOf(ko, "Digit1", "shift"), { slot: { t: "!", r: "x" }, from: "jis" });
  assert.deepEqual(L.slotOf(qwerty, "KeyA", "shift"), { slot: { t: "A", r: "l" }, from: "jis" });
  assert.deepEqual(L.slotOf(qwerty, "Digit0", "shift"), { slot: null, from: "jis" });
  assert.deepEqual(L.slotOf(qwerty, "KeyA", "alt"), { slot: null, from: "none" });
  assert.deepEqual(L.slotOf(qwerty, "Space", "base"), { slot: null, from: "none" });
});

test("a new layout of each way of reading fits the schema, and the engine takes it", () => {
  for (const read of L.READS) {
    const doc = L.fresh(schema, read, "en-mine");
    assert.deepEqual(L.validate(schema, doc), [], read);
    assert.deepEqual(errors(doc), [], read);
  }
});

test("changing the way of reading leaves a file the engine takes", () => {
  for (const from of stock) {
    for (const read of L.READS) {
      const doc = L.copy(from);
      doc.id = "en-changed";
      L.setRead(schema, doc, read);
      // What names an option or a part of the old way is the user's to mend; the shape is whole.
      const shape = L.validate(schema, doc).filter(p => !/^\/(rank|convert|actions|forms|options)/.test(p.path));
      assert.deepEqual(shape, [], `${from.id} as ${read}`);
    }
  }
  const doc = L.copy(stock.find(l => l.id === "ko-2-set"));
  const dropped = L.setRead(schema, doc, "direct");
  assert.equal(doc.kind, "muhenkan");
  assert.ok(dropped.includes("/hangul") && dropped.includes("/keys/KeyR/base/r"));
  assert.deepEqual(doc.keys.KeyR.base, { t: "ㄱ", r: "l" });
});

test("the least value of every part fits it", () => {
  for (const [name, node] of Object.entries(schema.properties)) {
    const value = L.blank(schema, node);
    const doc = { [name]: value };
    const told = L.validate({ ...schema, required: [], allOf: [] }, doc);
    assert.deepEqual(told, [], name);
  }
});

console.log(process.exitCode ? "FAIL" : `ok   ${passed} tests passed`);
