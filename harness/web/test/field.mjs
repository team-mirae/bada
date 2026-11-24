// The text field model (src/field.js) against the standard's §6.3, rule by rule, with replies
// written by hand: no engine is needed.
//   node harness/web/test/field.mjs
import assert from "node:assert/strict";
import { Field } from "../src/field.js";

let passed = 0, failed = 0;
function test(name, run) {
  try { run(); passed++; } catch (e) { failed++; console.log(`  FAIL ${name}\n      ${e.message.split("\n").join("\n      ")}`); }
}
const ok = fields => ({ ok: true, commit: "", replace: "marked", consumed: true, types: null, preedit: { text: "" }, menu: { rows: [] }, ...fields });
const field = (text = "", start = text.length, end = start) => { const f = new Field(); f.set(text, start, end); return f; };

test("a commit goes in at the caret, and the caret after it", () => {
  const f = field("ab", 1);
  f.apply(ok({ commit: "한" }));
  assert.deepEqual([f.text, f.start, f.end, f.marked], ["a한b", 2, 2, null]);
});
test("a commit replaces the selection when nothing is marked", () => {
  const f = field("abcd", 1, 3);
  f.apply(ok({ commit: "X" }));
  assert.deepEqual([f.text, f.start, f.end], ["aXd", 2, 2]);
});
test("a preedit is marked, and replaces the marked text after it", () => {
  const f = field("ab", 1);
  f.apply(ok({ preedit: { text: "ㅎ" } }));
  assert.deepEqual([f.text, f.marked, f.start], ["aㅎb", { start: 1, len: 1 }, 2]);
  f.apply(ok({ preedit: { text: "한국" } }));
  assert.deepEqual([f.text, f.marked, f.start], ["a한국b", { start: 1, len: 2 }, 3]);
});
test("a commit replaces the marked text, then the new preedit follows it", () => {
  const f = field("ab", 1);
  f.apply(ok({ preedit: { text: "한" } }));
  f.apply(ok({ commit: "한", preedit: { text: "ㄱ" } }));
  assert.deepEqual([f.text, f.marked, f.start], ["a한ㄱb", { start: 2, len: 1 }, 3]);
});
test("replace: selection leaves the marked text and replaces the selection", () => {
  const f = field("ab", 2);
  f.apply(ok({ preedit: { text: "한" } }));
  f.start = 0; f.end = 1;   // the user's selection, with text marked elsewhere
  f.apply(ok({ commit: "X", replace: "selection" }));
  assert.deepEqual([f.text, f.marked], ["Xb한", null]);
});
test("an empty preedit deletes the marked text", () => {
  const f = field("ab", 1);
  f.apply(ok({ preedit: { text: "한" } }));
  f.apply(ok({}));
  assert.deepEqual([f.text, f.marked, f.start, f.segments], ["ab", null, 1, []]);
});
test("the caret is placed in the preedit, counted in Unicode scalar values", () => {
  const f = field("");
  f.apply(ok({ preedit: { text: "😀あb", caret: 1 } }));
  assert.deepEqual([f.start, f.end], [2, 2], "😀 is two UTF-16 units and one scalar value");
  f.apply(ok({ preedit: { text: "😀あb" } }));
  assert.equal(f.start, 4, "a missing caret is the end");
});
test("segments become ranges of the field, each with its kind", () => {
  const f = field("x");
  f.apply(ok({ preedit: { text: "😀あinter", caret: 7, segments: [{ len: 2, kind: "fixed" }, { len: 3, kind: "picked" }, { len: 2, kind: "live" }] } }));
  assert.deepEqual(f.segments, [{ start: 1, len: 3, kind: "fixed" }, { start: 4, len: 3, kind: "picked" }, { start: 7, len: 2, kind: "live" }]);
});
test("no segments, or segments that don't add up: the whole preedit is live", () => {
  const f = field("");
  f.apply(ok({ preedit: { text: "abc" } }));
  assert.deepEqual(f.segments, [{ start: 0, len: 3, kind: "live" }]);
  f.apply(ok({ preedit: { text: "abc", segments: [{ len: 2, kind: "fixed" }] } }));
  assert.deepEqual(f.segments, [{ start: 0, len: 3, kind: "live" }]);
});
test("the menu and the direction are the reply's; a missing field is its default", () => {
  const f = field("");
  f.apply(ok({ preedit: { text: "か" }, menu: { rows: [{ text: "か", entry: 0, note: "ka", lang: "ja" }, { text: "カ", entry: 1 }], selected: 0, strip: true }, dir: "rtl" }));
  assert.deepEqual([f.menu.rows.length, f.menu.selected, f.menu.strip, f.dir], [2, 0, true, "rtl"]);
  f.apply({ ok: true });
  assert.deepEqual([f.menu, f.dir, f.text], [{ rows: [], selected: null, strip: false }, "ltr", ""]);
});
test("ok: false changes nothing", () => {
  const f = field("a");
  f.apply(ok({ preedit: { text: "한" }, menu: { rows: [{ text: "한", entry: 0 }], selected: 0 } }));
  const before = JSON.stringify(f);
  f.apply({ ok: false, error: { code: "bad-row", message: "" }, commit: "ignored", preedit: { text: "ignored" } });
  f.apply({ ok: false, error: { code: "a-code-from-the-future", message: "" } });
  assert.equal(JSON.stringify(f), before);
});
test("panic: the marked text is deleted and the menu closed", () => {
  const f = field("ab", 1);
  f.apply(ok({ preedit: { text: "한" }, menu: { rows: [{ text: "한", entry: 0 }], selected: 0 } }));
  f.apply({ ok: false, error: { code: "panic", message: "" } });
  assert.deepEqual([f.text, f.marked, f.menu.rows, f.start], ["ab", null, [], 1]);
});
test("an ask is answered once with a text event, and its reply applied", () => {
  const f = field("say abc now", 4, 7);
  const sent = [];
  const send = event => { sent.push(event); return ok({ commit: "ａｂｃ", replace: "selection", ask: { selection: "full" } }); };
  f.apply(ok({ ask: { selection: "full" } }), send);
  assert.deepEqual(sent, [{ type: "text", selection: "abc" }], "the answer's own ask is not answered: never deeper");
  assert.equal(f.text, "say ａｂｃ now");
});
test("the selection asked for is empty while text is marked", () => {
  const f = field("abc", 0, 3);
  f.apply(ok({ preedit: { text: "한" } }));
  assert.deepEqual(f.answer({ selection: "hanja" }), { type: "text", selection: "" });
});
test("the text around the caret is no longer than asked, in Unicode scalar values", () => {
  const f = field("a😀한국어bc", 5);   // the caret after 국: 😀 is two units
  assert.deepEqual(f.answer({ around: { before: 3, after: 2 } }), { type: "text", before: "😀한국", after: "어b" });
  assert.deepEqual(f.answer({ around: { before: 0, after: 0 } }), { type: "text", before: "", after: "" });
  f.apply(ok({ preedit: { text: "ㅎ" } }));
  assert.deepEqual(f.answer({ around: { before: 2, after: 1 } }), { type: "text", before: "한국", after: "어" }, "the marked text is not around the caret");
});
test("without a way to send, an ask is left alone", () => {
  const f = field("");
  f.apply(ok({ ask: { selection: "full" } }));
  assert.equal(f.text, "");
});
test("what a plain text field does with a key passed on", () => {
  const f = field("a😀b");
  f.key("Backspace"); f.key("Backspace");
  assert.equal(f.text, "a", "Backspace takes a whole character");
  f.insert("한글");
  f.key("ArrowLeft"); f.key("Delete");
  assert.deepEqual([f.text, f.start], ["a한", 2]);
  f.key("Home"); f.key("ArrowRight", { shift: true });
  assert.equal(f.selected, "a");
  f.insert("b");
  assert.equal(f.text, "b한");
  assert.equal(f.key("F5"), false);
});

console.log(`${failed ? "FAIL" : "ok  "} text field: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
