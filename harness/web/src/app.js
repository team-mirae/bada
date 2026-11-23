// bada in the browser: the page around the web harness (harness.js). It gives the harness the
// browser's keys, mirrors the harness's text field into a <textarea>, and draws what shows: the
// marked text, the menu, the state and a keyboard. Everything the keys do is the engine's and the
// key table's; nothing is decided here.

import { loadEngine } from "./load.js";
import { Harness } from "./harness.js";
import { drawKeyboard } from "./keyboard.js";
import { KEYBOARDS } from "./keyboards.js";
import { PRESET_NAMES, parseOn } from "./keytable.js";
import * as S from "./settings.js";
import { openOnboarding, openSettings } from "./settings-ui.js";

const $ = id => document.getElementById(id);
const ta = $("text"), backdrop = $("backdrop"), menuEl = $("menu"), board = $("board");
const MAC = /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent);
const coarse = matchMedia("(pointer: coarse)").matches;
const language = ["ko", "ja"].find(l => (navigator.language || "").startsWith(l)) ?? "en";

let h = null;            // the harness
let stock = {};          // the stock layouts' files, by id
let keys = {};           // code → key group on the keyboard drawn below
let boardOf = "";        // what that keyboard was drawn from
let modeOf = "";         // what the mode's buttons were made from
let modeClicks = [];     // what each of them does now
let ownOf = "";          // what this page's own choices were filled from
let helpOf = "";         // what the key table's list was written from
let published = "";      // the state as it was last published
let drawn = 0;           // counts refreshes
let osLock = null;       // the OS's Caps Lock at the last Caps Lock press the page handled
const latched = { shift: false, ctrl: false, alt: false, meta: false };

// ── Loading ────────────────────────────────────────────────────────────────────────────────────

/** Local storage, or a store that lasts as long as the page when the browser gives none. */
function storage() {
  try {
    localStorage.getItem(S.STORE.settings);
    return localStorage;
  } catch {
    warn("This browser keeps nothing for this page: the settings last until it is closed.");
    const kept = new Map();
    return { getItem: k => kept.get(k) ?? null, setItem: (k, v) => kept.set(k, String(v)), removeItem: k => kept.delete(k) };
  }
}

async function start() {
  const loaded = await loadEngine((got, total) => {
    $("progress").style.width = `${Math.min(100, (100 * got) / total).toFixed(1)}%`;
    $("progressText").textContent = `${(got / 1e6).toFixed(2)} of ${(total / 1e6).toFixed(2)} MB`;
  });
  stock = loaded.layouts;
  h = new Harness({ engine: loaded.engine, storage: storage(), platform: MAC ? "mac" : "pc", language });
  h.onchange = later;
  h.ononboarding = () => openOnboarding(ui);
  h.onwarn = warn;
  $("loading").hidden = true;
  $("settingsBtn").disabled = false;
  ta.disabled = false;
  ta.placeholder = "Type here.";
  if (coarse) ta.inputMode = "none";   // the keyboard below is the keyboard
  ta.focus();
  refresh();
  const wrong = h.published().harness.problems.filter(p => p.level === "error");
  if (wrong.length) warn(`What this browser keeps for bada has ${wrong.length === 1 ? "a problem" : wrong.length + " problems"}: open Settings to see. Nothing was changed.`);
  // A new install has no layout and no profile: the onboarding offers the two to start with (§8.3).
  else if (!h.profiles.length && !h.effective?.muhenkan?.layout) openOnboarding(ui);
}

/** What the dialogs need of the page (settings-ui.js). */
const ui = {
  get h() { return h; },
  get stock() { return stock; },
  language,
  refresh: () => { boardOf = ""; refresh(); },
  focus: () => ta.focus(),
};

$("settingsBtn").onclick = () => { if (h) { pull(); openSettings(ui); } };

function warn(text) {
  const w = $("warning");
  w.textContent = text;
  w.hidden = false;
  clearTimeout(warn.timer);
  warn.timer = setTimeout(() => { w.hidden = true; }, 9000);
}

// ── The text field: the harness's model, mirrored into the <textarea> ──────────────────────────

/** The browser's field → the model: its text and selection may have changed under the page. */
function pull() {
  const f = h.field;
  if (ta.value !== f.text && f.marked) {
    // The text changed under the marked text, and no flush came first: what is staged is dropped.
    f.marked = null;
    f.set(ta.value, ta.selectionStart, ta.selectionEnd);
    h.send({ type: "reset" });
    return;
  }
  f.set(ta.value, ta.selectionStart, ta.selectionEnd);
}

/** The model → the browser's field: one edit, the part between what stayed the same. */
function push() {
  const f = h.field, a = ta.value, b = f.text;
  if (a !== b) {
    let s = 0, ea = a.length, eb = b.length;
    while (s < ea && s < eb && a[s] === b[s]) s++;
    while (ea > s && eb > s && a[ea - 1] === b[eb - 1]) { ea--; eb--; }
    ta.setRangeText(b.slice(s, eb), s, ea, "end");
  }
  if (ta.selectionStart !== f.start || ta.selectionEnd !== f.end) ta.setSelectionRange(f.start, f.end);
}

const esc = t => t.replace(/[&<>]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" })[c]);
const KINDS = ["live", "fixed", "picked"];

/** The backdrop: the marked text's segments, each drawn in its own way; and the menu under them. */
function drawField() {
  const f = h.field, v = f.text, m = f.marked;
  const at = m ? m.start : f.start;
  let html = `${esc(v.slice(0, at))}<span id="anchor"></span>`;
  if (m) {
    for (const s of f.segments) html += `<span class="seg ${KINDS.includes(s.kind) ? s.kind : "live"}">${esc(v.slice(s.start, s.start + s.len))}</span>`;
    html += esc(v.slice(m.start + m.len));
  } else {
    html += esc(v.slice(at));
  }
  backdrop.innerHTML = html + "\n";
  backdrop.scrollTop = ta.scrollTop;
  drawMenu();
}

function drawMenu() {
  const { rows, selected, strip } = h.field.menu;
  if (!rows.length) {
    menuEl.hidden = true;
    menuEl.replaceChildren();
    return;
  }
  const rtl = h.field.dir === "rtl";
  menuEl.className = "menu" + (strip ? " strip" : "");
  menuEl.dir = h.field.dir;
  const several = new Set(rows.map(r => r.layout).filter(Boolean)).size > 1;
  menuEl.replaceChildren(...rows.map((row, i) => {
    const item = document.createElement("div");
    item.className = (strip ? "cell" : "row") + (i === selected ? " sel" : "");
    item.dataset.row = i;
    if (row.lang) item.lang = row.lang;   // the browser picks the font by it
    const text = document.createElement("span");
    text.textContent = row.text;
    item.append(text);
    if (strip) {
      // One row of single characters: 1–9 and 0 pick (the standard, §14).
      const digit = document.createElement("small");
      digit.textContent = i < 10 ? String((i + 1) % 10) : "";
      item.append(digit);
      return item;
    }
    const note = (t, lang) => { const n = document.createElement("span"); n.className = "note"; n.textContent = t; if (lang) n.lang = lang; item.append(n); };
    if (row.note) note(row.note, row.lang);
    if (several && row.layout) note(row.layout, "en");
    return item;
  }));
  menuEl.hidden = false;
  // Under the start of the marked text; at its end side when the text runs right to left.
  const anchor = $("anchor"), editor = $("editor").getBoundingClientRect();
  const a = anchor.nextElementSibling?.getClientRects()[0] || anchor.getBoundingClientRect();
  const width = menuEl.offsetWidth;
  const left = rtl ? a.right - editor.left - width + 12 : a.left - editor.left - 12;
  menuEl.style.left = `${Math.max(4, Math.min(left, editor.width - width - 4))}px`;
  menuEl.style.top = `${a.bottom - editor.top + 6}px`;
  const row = menuEl.children[selected ?? -1];
  if (row && !strip) row.scrollIntoView({ block: "nearest" });
}

// A click on a row picks it and commits it.
menuEl.addEventListener("pointerdown", e => {
  e.preventDefault();   // the focus stays in the text
  const item = e.target.closest("[data-row]");
  if (!item || !h) return;
  h.pick(Number(item.dataset.row));
  refresh();
});

ta.addEventListener("scroll", () => { backdrop.scrollTop = ta.scrollTop; if (h) drawMenu(); });
new ResizeObserver(() => h && drawField()).observe(ta);
// A click in the text, an edit by the browser, or leaving the field commits what is staged: the
// flush is applied before the field changes (§6.3).
ta.addEventListener("pointerdown", () => { if (h) { pull(); h.flush("click"); refresh(); } });
ta.addEventListener("beforeinput", () => { if (h) { h.flush("edit"); push(); } });
ta.addEventListener("input", () => { if (h) { pull(); refresh(); } });
ta.addEventListener("select", () => { if (h && !h.field.marked) { pull(); drawField(); } });
ta.addEventListener("focus", () => { if (h) { pull(); h.active = true; refresh(); } });
ta.addEventListener("blur", () => { if (h) { pull(); h.active = false; h.flush("blur"); refresh(); } });
ta.addEventListener("compositionstart", () => warn("The OS's own input method is composing. Switch it to a plain keyboard (ABC, or English) so that bada gets the keys."));

// ── Keys ───────────────────────────────────────────────────────────────────────────────────────

const pressOf = (e, code) => ({ code, ctrl: e.ctrlKey, alt: e.altKey, shift: e.shiftKey, meta: e.metaKey, repeat: e.repeat });
// Keys that are no press for an input method: locks and fn, and a key the browser can't name.
const NOT_KEYS = ["", "Unidentified", "Fn", "FnLock", "NumLock", "ScrollLock"];
const SAME_KEY = { NumpadEnter: "Enter", OSLeft: "MetaLeft", OSRight: "MetaRight" };
const codeOf = e => (NOT_KEYS.includes(e.code) ? null : SAME_KEY[e.code] ?? e.code);
const typing = e => h && !document.querySelector("dialog[open]") && (e.target === ta || e.target === document.body || board.contains(e.target));

function keyDown(e) {
  if (!typing(e)) return;
  keys[e.code]?.classList.add("on");
  if (e.code.startsWith("Shift")) board.classList.add("shift");
  if (e.isComposing || e.keyCode === 229) {
    warn("The OS's own input method is composing. Switch it to a plain keyboard (ABC, or English) so that bada gets the keys.");
    return;
  }
  const code = codeOf(e);
  if (!code) return;
  if (e.target !== ta) ta.focus();
  if (code === "CapsLock") osLock = e.getModifierState("CapsLock");
  pull();
  // What the OS's own layout would type: the browser types it when it is what the engine says (§6.4).
  const dealt = h.press(pressOf(e, code), { osKey: e.key });
  refresh();
  if (dealt) e.preventDefault();
}

function keyUp(e) {
  keys[e.code]?.classList.remove("on");
  if (e.code.startsWith("Shift") && !latched.shift) board.classList.remove("shift");
  const code = codeOf(e);
  if (!typing(e) || !code) return;
  pull();
  // Some browsers on a Mac report Caps Lock going off as a keyup alone. The OS's lock changed and
  // no keydown said so: this is a press too. (Elsewhere the lock may change between a press's
  // keydown and its keyup, so this is the Mac's rule only.)
  if (MAC && code === "CapsLock" && e.getModifierState("CapsLock") !== (osLock ?? true)) {
    osLock = e.getModifierState("CapsLock");
    h.press({ ...pressOf(e, code), repeat: false });
  }
  h.release(code);
  refresh();
}

addEventListener("keydown", keyDown);
addEventListener("keyup", keyUp);
addEventListener("blur", () => {
  Object.values(keys).forEach(g => g.classList.remove("on"));
  board.classList.remove("shift");
  h?.drop();
});

// Clicks on the keyboard below: a key press with no browser behind it. Modifiers latch for the next key.
const LATCH = { ShiftLeft: "shift", ShiftRight: "shift", ControlLeft: "ctrl", ControlRight: "ctrl", AltLeft: "alt", AltRight: "alt", MetaLeft: "meta", MetaRight: "meta" };
board.addEventListener("pointerdown", e => {
  const g = e.target.closest(".key");
  e.preventDefault();   // the focus stays in the text
  if (!g || !g.dataset.code || !h) return;
  const code = g.dataset.code;
  if (LATCH[code]) {
    latched[LATCH[code]] = !latched[LATCH[code]];
    drawLatched();
    return;
  }
  if (document.activeElement !== ta && !coarse) ta.focus();
  g.classList.add("on");
  setTimeout(() => g.classList.remove("on"), 120);
  pull();
  h.press({ code, ...latched, repeat: false }, { synthetic: true });
  h.release(code);
  for (const mod in latched) latched[mod] = false;
  refresh();
});

function drawLatched() {
  for (const [code, mod] of Object.entries(LATCH)) keys[code]?.classList.toggle("latched", latched[mod]);
  board.classList.toggle("shift", latched.shift);
  keys.CapsLock?.classList.toggle("latched", !!h?.caps);   // the Caps Lock light
}

// ── What shows around the field ────────────────────────────────────────────────────────────────

/** The mode's buttons: muhenkan, and each henkan profile by its name (§8.1). */
function drawMode() {
  const henkan = h.mode === "henkan";
  const items = [["Muhenkan", "無変換", !henkan, () => h.enter("muhenkan")]];
  for (const p of h.profiles) items.push([p.name, p.id === p.name ? "" : p.id, henkan && h.profile === p.id, () => h.enter("henkan", p.id)]);
  if (!h.profiles.length) items.push(["Henkan…", "変換", false, () => h.enter("henkan")]);
  // The buttons are made again only when the list changes: one that is being clicked stays.
  const from = JSON.stringify(items.map(([label, sub]) => [label, sub]));
  if (from !== modeOf) {
    modeOf = from;
    $("mode").replaceChildren(...items.map(([label, sub], i) => {
      const b = document.createElement("button");
      b.textContent = label;
      if (sub) { const s = document.createElement("span"); s.textContent = sub; b.append(s); }
      b.onclick = () => { pull(); modeClicks[i](); refresh(); ta.focus(); };
      return b;
    }));
  }
  modeClicks = items.map(item => item[3]);
  [...$("mode").children].forEach((b, i) => {
    b.classList.toggle("on", items[i][2]);
    b.setAttribute("aria-pressed", String(items[i][2]));
  });
}

/** The small panel (§6.7): mode, profile, what is staged; and the state as it is published. */
function drawState() {
  const state = h.published();
  const henkan = h.mode === "henkan";
  const profile = h.profiles.find(p => p.id === h.profile);
  const muhenkan = h.effective?.muhenkan?.layout;
  const items = [
    ["Mode", h.mode],
    ["Profile", h.profile == null ? "none" : `${profile?.name ?? "?"} (${h.profile})`],
    henkan ? ["Layouts", (profile?.layouts ?? []).map(id => h.nameOf(id)).join(" + ")] : ["Layout", muhenkan ? h.nameOf(muhenkan) : "none: the OS types"],
    ["Staged", JSON.stringify(state.staged)],
    ["Caps Lock", `${h.caps ? "on" : "off"} (${h.table.caps_lock}, ${h.table.caps_rule})`],
  ];
  const errors = state.harness.problems.filter(p => p.level === "error").length;
  if (state.harness.problems.length) items.push(["Problems", `${state.harness.problems.length}${errors ? "" : " (warnings)"}: see Settings`, "problem"]);
  $("state").replaceChildren(...items.map(([name, value, cls]) => {
    const div = document.createElement("div"), dt = document.createElement("dt"), dd = document.createElement("dd");
    dt.textContent = name;
    dd.textContent = value;
    if (cls) dd.className = cls;
    div.append(dt, dd);
    return div;
  }));
  // Published only when it changed. Reading it changes nothing, and nothing is read back from it.
  const json = JSON.stringify(state);
  if (json !== published) {
    published = json;
    $("bada-state").textContent = json;
    $("stateJson").textContent = JSON.stringify(state, null, 1);
    dispatchEvent(new CustomEvent("bada-state", { detail: JSON.parse(json) }));
  }
}

/** The keyboard drawn when the choice is automatic: the one the key preset is for. */
function boardName() {
  const chosen = S.own(h.file).keyboard;
  if (chosen !== "auto") return chosen;
  const preset = h.table.preset ?? (h.table.bindings.some(b => /Convert|KanaMode/.test(b.on ?? "")) ? "jis-pc" : "jis-mac");
  return preset === "jis-pc" ? "pc" : preset === "none" ? "apple" : "hhkb";
}

/** This page's own two choices, kept in the settings file under the harness's id. */
function drawOwn() {
  const own = S.own(h.file);
  const theirs = Array.isArray(h.file.keys?.bindings);
  const presets = theirs ? [["", "the settings' own bindings"]]
    : S.OWN.preset.map(v => [v, v === "auto" ? `automatic (${PRESET_NAMES[h.preset]})` : PRESET_NAMES[v]]);
  const boards = S.OWN.keyboard.map(v => [v, v === "auto" ? `automatic (${KEYBOARDS[boardName()].name})` : KEYBOARDS[v].name]);
  // Filled again only when a choice changed: a list that is open stays open.
  const from = JSON.stringify([own, theirs, presets, boards]);
  if (from === ownOf) return;
  ownOf = from;
  const fill = (select, value, options) => {
    select.replaceChildren(...options.map(([v, label]) => { const o = document.createElement("option"); o.value = v; o.textContent = label; return o; }));
    select.value = value;
  };
  fill($("preset"), theirs ? "" : own.preset, presets);
  $("preset").disabled = theirs;
  fill($("keyboard"), own.keyboard, boards);
}

for (const name of ["preset", "keyboard"]) {
  $(name).onchange = e => {
    pull();
    const result = h.setOwn(name, e.target.value);
    if (!result.ok) warn(`Not changed: ${result.problems.map(p => p.message).join("; ")}`);
    refresh();
    ta.focus();
  };
}

function drawBoard() {
  const profile = h.profiles.find(p => p.id === h.profile);
  const muhenkan = h.effective?.muhenkan?.layout;
  const ids = h.mode === "henkan" ? profile?.layouts ?? [] : muhenkan ? [muhenkan] : [];
  const custom = h.customLayouts;
  const files = ids.map(id => custom[id] ?? stock[id]).filter(Boolean);
  // With no layout the keys show what is printed on them.
  const layouts = files.length ? files : [{ id: "jis", name: { en: "JIS" }, keys: {} }];
  const bound = [...new Set(h.table.bindings.flatMap(b => { const p = parseOn(b.on); return p ? p.pair ?? [p.code] : []; }))];
  const name = boardName();
  const from = JSON.stringify([name, layouts.map(l => l.id), bound]);
  if (from !== boardOf) {
    boardOf = from;
    keys = drawKeyboard(board, { name, layouts, bound });
  }
  drawLatched();
}

const KEY_NAMES = { Lang1: "かな", Lang2: "英数", Convert: "変換", NonConvert: "無変換", KanaMode: "カタカナ", Backquote: "半角/全角", CapsLock: "Caps Lock" };
const MOD_NAMES = MAC ? { ctrl: "⌃", alt: "⌥", shift: "⇧", meta: "⌘" } : { ctrl: "Ctrl+", alt: "Alt+", shift: "Shift+", meta: "Win+" };

/** A binding's key, as people call it. */
function keyName(on) {
  const press = parseOn(on);
  if (!press) return String(on);
  if (press.pair) return press.pair.map(code => KEY_NAMES[code] ?? code).join(" + ");
  return Object.keys(MOD_NAMES).filter(mod => press[mod]).map(mod => MOD_NAMES[mod]).join("") + (KEY_NAMES[press.code] ?? press.code.replace(/^(Key|Digit)/, ""));
}

/** The key table in effect, as it is: each binding's key, its actions and when it holds (§9.1). */
function drawHelp() {
  const from = JSON.stringify([h.table.preset, h.table.bindings]);
  if (from === helpOf) return;
  helpOf = from;
  const list = document.createElement("ul");
  for (const b of h.table.bindings) {
    const li = document.createElement("li"), kbd = document.createElement("kbd");
    kbd.textContent = keyName(b.on);
    li.append(kbd, ` ${b.do.join(", else ")}${b.when ? ` (in ${b.when})` : ""}`);
    list.append(li);
  }
  const hint = document.createElement("div");
  hint.textContent = "A language key your keyboard lacks can be clicked on the keyboard above, and the mode's buttons are at the top. "
    + "In henkan: Tab and ↓ the next row, ⇧Tab and ↑ the previous; Space, Enter and Esc commit.";
  const title = document.createElement("div");
  title.textContent = h.table.preset ? `Keys: the preset "${PRESET_NAMES[h.table.preset]}" (the settings have no bindings of their own)` : "Keys: the settings' own bindings";
  $("help").replaceChildren(title, list, hint);
}

function refresh() {
  drawn++;
  push();
  drawMode();
  drawField();
  drawState();
  drawOwn();
  drawBoard();
  drawHelp();
}

/** A refresh once the harness is done with what it is doing. */
function later() {
  const at = drawn;
  queueMicrotask(() => { if (drawn === at) refresh(); });
}

start().catch(err => {
  $("progressText").textContent = `bada couldn't start: ${err.message}`;
  console.error(err);
});
