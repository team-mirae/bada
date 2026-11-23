// The keyboards the page can draw, in key units (1u = one letter key). Each key:
// [code, x, y, w, h, label, shape]. code is the KeyboardEvent.code a click on it presses; null for
// a key no browser sees (fn). "ENTER" is the ISO-shaped Return (1.5u on the Q row, 1.25u below).

const row = (y, x0, codes) => {
  let x = x0;
  return codes.map(([c, w = 1, label]) => { const k = [c, x, y, w, 1, label]; x += w; return k; });
};
const NUM = ["Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8", "Digit9", "Digit0", "Minus", "Equal", "IntlYen"].map(c => [c]);
const QROW = ["KeyQ", "KeyW", "KeyE", "KeyR", "KeyT", "KeyY", "KeyU", "KeyI", "KeyO", "KeyP", "BracketLeft", "BracketRight"].map(c => [c]);
const AROW = ["KeyA", "KeyS", "KeyD", "KeyF", "KeyG", "KeyH", "KeyJ", "KeyK", "KeyL", "Semicolon", "Quote", "Backslash"].map(c => [c]);
const ZROW = ["KeyZ", "KeyX", "KeyC", "KeyV", "KeyB", "KeyN", "KeyM", "Comma", "Period", "Slash", "IntlRo"].map(c => [c]);

// HHKB Professional JIS, 15u x 5u. It has 半角/全角, 英数 and かな, and no Caps Lock key.
const hhkb = [
  ...row(0, 0, [["Escape", 1, "Esc"], ...NUM, ["Backspace", 1, "Delete"]]),
  ...row(1, 0, [["Tab", 1.5, "Tab"], ...QROW]),
  ...row(2, 0, [["ControlLeft", 1.75, "Control"], ...AROW]),
  ...row(3, 0, [["ShiftLeft", 2, "Shift"], ...ZROW, ["ArrowUp", 1, "↑"], ["ShiftRight", 1, "Shift"]]),
  ...row(4, 0, [[null, 1, "fn"], ["GAP", 0.25], ["Backquote", 1, "半角/全角"], ["AltLeft", 1, "⌥"], ["MetaLeft", 1, "⌘"],
    ["Lang2", 1, "英数"], ["Space", 2.5, ""], ["Lang1", 1, "かな"], ["MetaRight", 1, "⌘"], ["AltRight", 1, "⌥"],
    [null, 1, "fn"], ["GAP", 0.25], ["ArrowLeft", 1, "←"], ["ArrowDown", 1, "↓"], ["ArrowRight", 1, "→"]]),
  ["Enter", 13.5, 1, 1.5, 2, "Return", "ENTER"],
];

// Apple-style JIS (a MacBook), 14.5u: 英数 and かな beside Space, Caps Lock bottom left, no 半角/全角.
const apple = [
  ...row(0, 0, [["Digit1", 1.5], ...NUM.slice(1), ["Backspace", 1, "delete"]]),
  ...row(1, 0, [["Tab", 1, "tab"], ...QROW]),
  ...row(2, 0, [["ControlLeft", 1.25, "control"], ...AROW]),
  ...row(3, 0, [["ShiftLeft", 1.75, "shift"], ...ZROW, ["ShiftRight", 1.75, "shift"]]),
  ...row(4, 0, [["CapsLock", 1, "caps"], ["AltLeft", 1, "⌥"], ["MetaLeft", 1, "⌘"], ["Lang2", 1, "英数"], ["Space", 4.5, ""],
    ["Lang1", 1, "かな"], ["MetaRight", 1, "⌘"], [null, 1, "fn"], ["ArrowLeft", 1, "←"]]),
  ["ArrowUp", 12.5, 4, 1, 0.5, "↑"], ["ArrowDown", 12.5, 4.5, 1, 0.5, "↓"], ["ArrowRight", 13.5, 4, 1, 1, "→"],
  ["Enter", 13, 1, 1.5, 2, "return", "ENTER"],
];

// A PC's JIS keyboard (OADG 109's main block), 15u: 半角/全角 top left, 無変換, 変換 and カタカナ
// beside Space, Caps Lock on the home row.
const pc = [
  ...row(0, 0, [["Backquote", 1, "半角/全角"], ...NUM, ["Backspace", 1, "BS"]]),
  ...row(1, 0, [["Tab", 1.5, "Tab"], ...QROW]),
  ...row(2, 0, [["CapsLock", 1.75, "Caps Lock"], ...AROW]),
  ...row(3, 0, [["ShiftLeft", 2.25, "Shift"], ...ZROW, ["ShiftRight", 1.75, "Shift"]]),
  ...row(4, 0, [["ControlLeft", 1.5, "Ctrl"], ["MetaLeft", 1, "Win"], ["AltLeft", 1.25, "Alt"], ["NonConvert", 1.25, "無変換"],
    ["Space", 3, ""], ["Convert", 1.25, "変換"], ["KanaMode", 1.25, "カタカナ"], ["AltRight", 1, "Alt"],
    ["ArrowLeft", 0.875, "←"], ["ArrowUp", 0.875, "↑"], ["ArrowDown", 0.875, "↓"], ["ArrowRight", 0.875, "→"]]),
  ["Enter", 13.5, 1, 1.5, 2, "Enter", "ENTER"],
];

const clean = keys => keys.filter(k => k[0] !== "GAP");
export const KEYBOARDS = {
  hhkb: { name: "HHKB JIS", width: 15, height: 5, keys: clean(hhkb) },
  apple: { name: "Apple-style JIS", width: 14.5, height: 5, keys: clean(apple) },
  pc: { name: "PC JIS", width: 15, height: 5, keys: clean(pc) },
};

// The legends printed on a JIS keyboard's 48 typing keys: [plain, shifted]. Only for drawing a
// slot that a layout leaves to the JIS table (the standard, §7.2). What a key types is always the
// engine's `types`, never this.
export const JIS = {
  Digit1: ["1", "!"], Digit2: ["2", "\""], Digit3: ["3", "#"], Digit4: ["4", "$"], Digit5: ["5", "%"], Digit6: ["6", "&"],
  Digit7: ["7", "'"], Digit8: ["8", "("], Digit9: ["9", ")"], Digit0: ["0", ""], Minus: ["-", "="], Equal: ["^", "~"],
  IntlYen: ["¥", "|"], BracketLeft: ["@", "`"], BracketRight: ["[", "{"], Semicolon: [";", "+"], Quote: [":", "*"],
  Backslash: ["]", "}"], Comma: [",", "<"], Period: [".", ">"], Slash: ["/", "?"], IntlRo: ["\\", "_"],
};
for (const c of "QWERTYUIOPASDFGHJKLZXCVBNM") JIS["Key" + c] = [c.toLowerCase(), c];
