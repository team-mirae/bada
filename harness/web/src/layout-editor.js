// The layout editor's page (layout.html): one bada-layout/3 file, edited field by field. Every
// field is drawn from the layout schema (layout.schema.json, a copy of docs/schemas/layout.schema.json)
// with the schema's own words beside it, so the page has no list of fields to keep up. The keys
// have a keyboard of their own. The draft is checked by the schema as it is typed and by the
// engine a moment later, and can be tried in a text field at the foot of the page.
//
// For a program that drives the page: every control has `data-path`, the JSON pointer of what it
// edits, and `window.badaLayoutEditor` does the same without the DOM (see `api` below).

import { Field } from "./field.js";
import * as L from "./layout-model.js";
import { loadEngine } from "./load.js";
import { STORE } from "./settings.js";

const DRAFT = "bada.web.layout-draft";   // the layout being edited, as text: it outlives the page
const $ = id => document.getElementById(id);
const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);

function h(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (k === "value") e.value = v;
    else if (k === "checked") e.checked = !!v;
    else e.setAttribute(k, v === true ? "" : v);
  }
  e.append(...children.flat(Infinity).filter(c => c != null && c !== false));
  return e;
}

// ── What the page holds ────────────────────────────────────────────────────────────────────────

let schema = null;        // the layout schema
let engine = null;        // the engine, for checking and for trying
let stock = {};           // the stock layouts' files, by id
let doc = null;           // the layout being edited
let told = [];            // what the schema says is wrong: [{path, message}]
let heard = [];           // what the engine says: [{path, code, message, level}]
let fed = null;           // the id the engine's session has the draft under, for trying
const past = [];          // earlier drafts, as text, for Undo
let pastAt = { path: null, time: 0 };
const ui = { key: "KeyA", filters: {}, note: "" };

const storage = (() => {
  try { localStorage.getItem(DRAFT); return localStorage; } catch {
    const kept = new Map();
    return { getItem: k => kept.get(k) ?? null, setItem: (k, v) => kept.set(k, String(v)), removeItem: k => kept.delete(k) };
  }
})();
/** The layouts saved in this browser, which the web page feeds its engine: `{id: layout}`. */
function saved() {
  try { const kept = JSON.parse(storage.getItem(STORE.layouts) ?? "{}"); return isObject(kept) ? kept : {}; } catch { return {}; }
}

// ── Changing the draft ─────────────────────────────────────────────────────────────────────────

/** Keeps the draft as it is now, for Undo. Typing in one field is one step. */
function remember(path = null) {
  const now = Date.now(), text = JSON.stringify(doc);
  if (path !== null && path === pastAt.path && now - pastAt.time < 1500) { pastAt.time = now; return; }
  pastAt = { path, time: now };
  if (past[past.length - 1] !== text) past.push(text);
  if (past.length > 200) past.shift();
}

/** A value typed into a field: the draft changes, the page stays as it is drawn. */
function put(path, value) {
  remember(L.pointer(path));
  L.set(doc, path, value);
  after(false);
}
/** A change to what the file has (a part added or removed, a form chosen): the page is drawn again. */
function reshape(change, note = "") {
  remember();
  change();
  ui.note = note;
  after(true);
}
function after(redraw) {
  storage.setItem(DRAFT, L.print(doc));
  if (redraw) draw();
  mirror();
  check();
}

/** Replaces the draft with another file. */
function load(value, note) {
  if (doc) remember();
  doc = L.copy(value);
  ui.filters = {};
  ui.note = note ?? "";
  after(true);
}
function undo() {
  const text = past.pop();
  if (text == null) return false;
  doc = JSON.parse(text);
  pastAt = { path: null, time: 0 };
  ui.note = "Undone.";
  after(true);
  return true;
}

// ── Checking ───────────────────────────────────────────────────────────────────────────────────

let checking = 0;
function check() {
  told = L.validate(schema, doc);
  say();
  clearTimeout(checking);
  checking = setTimeout(ask, 200);
}
/** Asks the engine what it makes of the draft, without taking it; then gives it to the try-it field. */
function ask() {
  if (!engine) return;
  const reply = engine.send(null, { type: "layout", value: doc, dry: true });
  heard = reply.problems ?? [];
  if (!reply.ok && !heard.length) heard = [{ path: "", code: reply.error?.code ?? "invalid", message: reply.error?.message ?? "the engine refused the layout", level: "error" }];
  say();
  feed();
}
const errors = () => heard.filter(p => p.level === "error");

/** Writes what is wrong: the list at the top, and a line under each field it is about. */
function say() {
  const list = $("problems"), warnings = heard.filter(p => p.level !== "error");
  const fits = !told.length, takes = !errors().length;
  $("status").className = "status " + (fits && takes ? (warnings.length ? "warn" : "ok") : "bad");
  $("status").textContent = !fits ? `${told.length} problem${told.length > 1 ? "s" : ""} with the format`
    : !engine ? "Fits the format. Loading the engine…"
    : !takes ? `The engine refuses it: ${errors().length} error${errors().length > 1 ? "s" : ""}`
    : warnings.length ? `The engine takes it, with ${warnings.length} warning${warnings.length > 1 ? "s" : ""}` : "The engine takes it";
  const go = path => h("a", { href: "#", class: "mono", onclick: e => { e.preventDefault(); reveal(path); } }, path || "(the whole file)");
  list.replaceChildren(
    ...told.map(p => h("li", { class: "error", "data-problem": p.path }, "format: ", go(p.path), " ", p.message)),
    ...heard.map(p => h("li", { class: p.level === "error" ? "error" : "warning", "data-problem": p.path }, `engine, ${p.level} (${p.code}): `, h("span", { class: "mono" }, p.path || "(the whole file)"), " ", p.message)));
  for (const e of document.querySelectorAll("[data-said]")) { e.textContent = ""; e.closest("[data-field]")?.classList.remove("bad"); }
  for (const p of told) {
    // The field itself, or the nearest part around it that is drawn.
    let path = L.parse(p.path), at = null;
    for (; !at; path = path.slice(0, -1)) {
      at = document.querySelector(`[data-said="${CSS.escape(L.pointer(path))}"]`);
      if (!path.length) break;
    }
    if (!at) continue;
    at.textContent += (at.textContent ? " · " : "") + (at.dataset.said === p.path ? "" : p.path + " ") + p.message;
    at.closest("[data-field]")?.classList.add("bad");
  }
  $("save").disabled = !fits || !takes || !engine;
  const mine = saved(), id = doc.id;
  $("save").textContent = stock[id] ? "Save: replaces the stock layout" : mine[id] ? "Save over my layout" : "Save in this browser";
  $("remove").hidden = !mine[id];
  $("remove").textContent = stock[id] ? "Remove my copy: the stock one comes back" : "Remove from this browser";
  $("undo").disabled = !past.length;
  $("note").textContent = ui.note;
}

/** Scrolls to the field a pointer names, and puts the caret in it. */
function reveal(path) {
  let at = null;
  for (let p = L.parse(path); !at; p = p.slice(0, -1)) {
    if (p[0] === "keys" && p[1]) { ui.key = p[1]; drawKeys(); }
    at = document.querySelector(`[data-field="${CSS.escape(L.pointer(p))}"]`);
    if (!p.length) break;
  }
  if (!at) return false;
  at.scrollIntoView({ block: "center", behavior: "smooth" });
  at.querySelector("input, select, textarea, button")?.focus({ preventScroll: true });
  return true;
}

// ── A field, drawn from its schema ─────────────────────────────────────────────────────────────

const control = (path, extra = {}) => { const p = L.pointer(path); return { "data-path": p, name: p, "aria-describedby": "about:" + p, ...extra }; };
const titleOf = (node, path) => node.title ?? String(path[path.length - 1] ?? "");
const scalar = node => ["string", "number", "integer", "boolean"].includes(node.type) || node.enum || Object.hasOwn(node, "const");

/** What the schema says of a field: what it does, what it may hold, and where it is in the file. */
function about(path, node, required) {
  const p = L.pointer(path);
  return h("div", { class: "about", id: "about:" + p },
    node.description ? h("p", {}, node.description) : null,
    h("p", { class: "schema" }, h("span", { class: "mono" }, p), " · ", L.limits(schema, node) || "any value", required ? " · required" : " · optional"),
    h("p", { class: "said", "data-said": p, role: "alert" }));
}

/** The control of a value that is one thing: a text, a number, a choice, a switch. */
function input(path, node, value, label) {
  const attrs = control(path, { "aria-label": label });
  if (Object.hasOwn(node, "const")) return h("input", { ...attrs, type: "text", class: "mono", value: String(value ?? node.const), readonly: true });
  if (node.enum) {
    const known = node.enum.includes(value);
    return h("select", { ...attrs, onchange: e => put(path, node.enum[Number(e.target.value)]) },
      known ? null : h("option", { value: "", selected: true }, `${JSON.stringify(value)} (not allowed)`),
      node.enum.map((v, i) => h("option", { value: i, selected: v === value }, String(v))));
  }
  if (node.type === "boolean") return h("input", { ...attrs, type: "checkbox", checked: value === true, onchange: e => put(path, e.target.checked) });
  if (node.type === "number" || node.type === "integer") {
    return h("input", { ...attrs, type: "number", step: node.type === "integer" ? 1 : "any", min: node.minimum, max: node.maximum, value: typeof value === "number" ? String(value) : "",
      oninput: e => { const n = e.target.valueAsNumber; e.target.setAttribute("aria-invalid", String(!Number.isFinite(n))); if (Number.isFinite(n)) put(path, n); } });
  }
  const long = path[path.length - 1] === "note" || String(value ?? "").length > 60;
  return h(long ? "textarea" : "input", { ...attrs, ...(long ? { rows: 2 } : { type: "text", size: Math.max(6, Math.min(40, [...String(value ?? "")].length + 2)) }), spellcheck: "false", autocomplete: "off",
    value: typeof value === "string" ? value : JSON.stringify(value), oninput: e => put(path, e.target.value) });
}

/** A field with its name, its words and its control. `required`: the part it is in can't be without it. */
function field(path, node, value, required) {
  const p = L.pointer(path), name = String(path[path.length - 1]), title = titleOf(node, path);
  const head = h("div", { class: "head" },
    h("label", { for: "f:" + p }, title), title === name ? null : h("code", {}, name),
    required ? null : h("button", { class: "quiet", "aria-label": `Remove ${p}`, "data-remove": p, title: "Take this field out of the file", onclick: () => reshape(() => L.remove(doc, path), `Removed ${p}.`) }, "Remove"));
  const body = draw1(path, node, value, title);
  if (body.matches?.("input, select, textarea")) body.id = "f:" + p;
  return h("div", { class: "field" + (scalar(node) ? " one" : ""), "data-field": p }, head, body, about(path, node, required));
}

/** A field the file doesn't have: what it would be, and the button that adds it. */
function absent(path, node, required) {
  const p = L.pointer(path), title = titleOf(node, path), name = String(path[path.length - 1]);
  return h("div", { class: "field absent", "data-field": p },
    h("div", { class: "head" },
      h("button", { "data-add": p, "aria-label": `Add ${p}`, onclick: () => reshape(() => L.set(doc, path, L.blank(schema, node)), `Added ${p}.`) }, "+ Add"),
      h("span", { class: "name" }, title), title === name ? null : h("code", {}, name), required ? h("span", { class: "pill bad" }, "required") : h("span", { class: "pill" }, "not set")),
    about(path, node, required));
}

/** The control, or the part, of any value. */
function draw1(path, node, value, label) {
  node = L.deref(schema, node);
  if (node.oneOf) return forms(path, node, value, label);
  if (scalar(node)) return input(path, node, value, label);
  if (node.type === "array") return list(path, node, value, label);
  if (node.type === "object" && node.properties) return part(path, node, value);
  if (node.type === "object") return map(path, node, value, label);
  return input(path, { type: "string" }, value, label);
}

/** An object with named fields: each one, in the schema's order; then what the format doesn't have. */
function part(path, node, value, skip = []) {
  if (!isObject(value)) return wrong(path, value, "an object");
  const required = node.required ?? [];
  const names = Object.keys(node.properties).filter(k => !skip.includes(k));
  return h("div", { class: "part" },
    names.filter(k => Object.hasOwn(value, k)).map(k => field([...path, k], L.deref(schema, node.properties[k]), value[k], required.includes(k))),
    Object.keys(value).filter(k => !Object.hasOwn(node.properties, k)).map(k => unknown([...path, k], value[k])),
    names.filter(k => !Object.hasOwn(value, k)).map(k => absent([...path, k], L.deref(schema, node.properties[k]), required.includes(k))));
}

/** Something the format has no place for, or a value of the wrong kind: shown as JSON, to mend or remove. */
function unknown(path, value) {
  const p = L.pointer(path);
  return h("div", { class: "field bad", "data-field": p },
    h("div", { class: "head" }, h("span", { class: "name mono" }, String(path[path.length - 1])),
      h("button", { class: "quiet", "data-remove": p, "aria-label": `Remove ${p}`, onclick: () => reshape(() => L.remove(doc, path), `Removed ${p}.`) }, "Remove")),
    raw(path, value), h("div", { class: "about" }, h("p", { class: "said", "data-said": p, role: "alert" })));
}
function wrong(path, value, wanted) {
  return h("div", { class: "wrong" }, h("p", { class: "muted" }, `This must be ${wanted}. As JSON:`), raw(path, value),
    h("button", { onclick: () => reshape(() => L.set(doc, path, L.blank(schema, L.schemaAt(schema, doc, path) ?? {})), `Reset ${L.pointer(path)}.`) }, "Start it again"));
}
/** Any value as JSON in a box: taken when it parses. */
function raw(path, value) {
  return h("textarea", { ...control(path), class: "mono", rows: 2, spellcheck: "false", value: JSON.stringify(value),
    onchange: e => { try { const v = JSON.parse(e.target.value); reshape(() => L.set(doc, path, v)); } catch { e.target.setAttribute("aria-invalid", "true"); } } });
}

/** A value that takes one of several forms: which, and that form's fields. */
function forms(path, node, value, label) {
  const at = L.variantOf(schema, node, value), branches = node.oneOf.map(b => L.deref(schema, b));
  const name = (b, i) => b.title ?? (b.type === "null" ? "Nothing (null)" : `Form ${i + 1}`);
  const choose = i => reshape(() => {
    const next = L.blank(schema, branches[i]);
    // What the two forms share is kept: an action's id and label, a conversion's data.
    if (isObject(next) && isObject(value)) for (const k of Object.keys(branches[i].properties ?? {})) {
      const to = L.deref(schema, branches[i].properties[k]);
      if (Object.hasOwn(value, k) && !Object.hasOwn(to, "const") && !L.validate({ ...to, $defs: schema.$defs }, value[k]).length) next[k] = value[k];
    }
    L.set(doc, path, next);
  }, `${L.pointer(path)} is now ${name(branches[i], i)}.`);
  const p = L.pointer(path);
  const pick = h("label", { class: "form" }, "Form ",
    h("select", { "data-form": p, "aria-label": `The form of ${p}`, onchange: e => choose(Number(e.target.value)) }, branches.map((b, i) => h("option", { value: i, selected: i === at }, name(b, i)))),
    branches[at].description ? h("span", { class: "muted" }, " ", branches[at].description) : null);
  const body = branches[at].type === "null" ? null : branches[at].type === "object" && branches[at].properties ? part(path, branches[at], value) : draw1(path, branches[at], value, label);
  return h("div", { class: "forms" }, pick, body);
}

/** A list. Short values sit in a row; parts stack, each with its place and its buttons. */
function list(path, node, value, label) {
  if (!Array.isArray(value)) return wrong(path, value, "a list");
  const items = L.deref(schema, node.items ?? {}), p = L.pointer(path);
  const move = (i, by) => reshape(() => { const [x] = value.splice(i, 1); value.splice(i + by, 0, x); });
  const drop = i => reshape(() => value.splice(i, 1), `Removed ${p}/${i}.`);
  const buttons = (i, [back, on]) => [
    h("button", { class: "quiet", disabled: i === 0, "aria-label": `Move ${p}/${i} earlier`, "data-move": `${p}/${i}`, onclick: () => move(i, -1) }, back),
    h("button", { class: "quiet", disabled: i === value.length - 1, "aria-label": `Move ${p}/${i} later`, onclick: () => move(i, 1) }, on),
    h("button", { class: "quiet", "aria-label": `Remove ${p}/${i}`, "data-remove": `${p}/${i}`, onclick: () => drop(i) }, "×")];
  // What can still be added: of a choice that may come once, the ones not there yet.
  const free = items.enum && node.uniqueItems ? items.enum.filter(v => !value.includes(v)) : null;
  const add = kind => reshape(() => value.push(kind ?? L.blank(schema, items)), `Added ${p}/${value.length}.`);
  if (scalar(items)) {
    return h("div", { class: "chips", "data-list": p },
      value.map((v, i) => h("span", { class: "chip" }, input([...path, i], items, v, `${label} ${i + 1}`), buttons(i, ["◀", "▶"]))),
      free ? (free.length ? h("select", { "data-add": p, "aria-label": `Add to ${p}`, onchange: e => { if (e.target.value !== "") add(free[Number(e.target.value)]); } }, h("option", { value: "" }, "+ Add…"), free.map((v, i) => h("option", { value: i }, String(v)))) : null)
        : h("button", { "data-add": p, "aria-label": `Add to ${p}`, onclick: () => add() }, "+ Add"),
      value.length ? null : h("span", { class: "muted" }, "empty"));
  }
  const kinds = items.oneOf?.map(b => L.deref(schema, b));
  return h("div", { class: "stack", "data-list": p },
    value.map((v, i) => h("div", { class: "item", "data-field": `${p}/${i}` },
      h("div", { class: "head" }, h("span", { class: "name" }, `${i + 1}`), h("span", { class: "muted mono" }, `${p}/${i}`), buttons(i, ["▲", "▼"])),
      draw1([...path, i], items, v, `${label} ${i + 1}`),
      h("p", { class: "said", "data-said": `${p}/${i}`, role: "alert" }))),
    kinds
      ? h("select", { "data-add": p, "aria-label": `Add to ${p}`, onchange: e => { if (e.target.value !== "") reshape(() => value.push(L.blank(schema, kinds[Number(e.target.value)])), `Added ${p}/${value.length}.`); } },
        h("option", { value: "" }, "+ Add…"), kinds.map((b, i) => h("option", { value: i }, `${b.title ?? `Form ${i + 1}`}: ${b.description ?? ""}`)))
      : h("button", { "data-add": p, "aria-label": `Add to ${p}`, onclick: () => add() }, "+ Add"));
}

/** A table of names and values (spellings, jamo lists, a name by language). A name can be changed in place. */
function map(path, node, value, label) {
  if (!isObject(value)) return wrong(path, value, "an object");
  const of = L.deref(schema, node.additionalProperties ?? {}), p = L.pointer(path), keys = Object.keys(value);
  const rename = (from, to, box) => {
    if (to === from) return;
    if (to === "" || Object.hasOwn(value, to)) { box.value = from; box.setAttribute("aria-invalid", "true"); return; }
    reshape(() => {
      const entries = Object.entries(value).map(([k, v]) => [k === from ? to : k, v]);
      for (const k of Object.keys(value)) delete value[k];
      Object.assign(value, Object.fromEntries(entries));
    }, `Renamed ${p}/${from} to ${to}.`);
  };
  const small = scalar(of) || (of.type === "array" && scalar(L.deref(schema, of.items ?? {})));
  const row = k => h("div", { class: "entry" + (small ? "" : " big"), "data-field": L.pointer([...path, k]), "data-entry": k.toLowerCase() + " " + JSON.stringify(value[k]).toLowerCase() },
    h("input", { type: "text", class: "mono key", value: k, size: Math.max(3, Math.min(16, [...k].length + 1)), spellcheck: "false", "aria-label": `The name of ${p}/${k}`, "data-key": L.pointer([...path, k]), onchange: e => rename(k, e.target.value, e.target) }),
    h("span", { class: "muted" }, "→"),
    draw1([...path, k], of, value[k], `${label} ${k}`),
    h("button", { class: "quiet", "aria-label": `Remove ${L.pointer([...path, k])}`, "data-remove": L.pointer([...path, k]), onclick: () => reshape(() => { delete value[k]; }, `Removed ${p}/${k}.`) }, "×"),
    h("span", { class: "said", "data-said": L.pointer([...path, k]), role: "alert" }));
  const rows = keys.map(row);
  const name = h("input", { type: "text", class: "mono key", size: 8, placeholder: "name", spellcheck: "false", "aria-label": `A new name in ${p}`, "data-new": p });
  const add = () => {
    const k = name.value;
    if (k === "" || Object.hasOwn(value, k)) { name.setAttribute("aria-invalid", "true"); name.focus(); return; }
    ui.filters[p] = "";
    reshape(() => { value[k] = L.blank(schema, of); }, `Added ${p}/${k}.`);
    document.querySelector(`[data-field="${CSS.escape(L.pointer([...path, k]))}"] [data-path]`)?.focus();
  };
  name.addEventListener("keydown", e => { if (e.key === "Enter") { e.preventDefault(); add(); } });
  const count = h("span", { class: "muted" });
  const filter = text => {
    ui.filters[p] = text;
    const terms = text.toLowerCase().split(/\s+/).filter(Boolean);
    let shown = 0;
    for (const r of rows) { r.hidden = !terms.every(t => r.dataset.entry.includes(t)); if (!r.hidden) shown++; }
    count.textContent = terms.length ? `${shown} of ${rows.length}` : `${rows.length} entries`;
  };
  const search = keys.length > 12 ? h("input", { type: "search", placeholder: "Find a name or a value…", value: ui.filters[p] ?? "", "aria-label": `Find in ${p}`, oninput: e => filter(e.target.value) }) : null;
  filter(search ? ui.filters[p] ?? "" : "");
  return h("div", { class: "map", "data-map": p },
    search ? h("div", { class: "row" }, search, count) : null,
    h("div", { class: "entries" + (small ? " small" : "") }, rows),
    h("div", { class: "row" }, name, h("button", { "data-add": p, "aria-label": `Add the new name to ${p}`, onclick: add }, "+ Add")));
}

// ── The keys ───────────────────────────────────────────────────────────────────────────────────

const LEGEND = { Space: "Space" };

/** The keyboard, and the layers of the key that is chosen on it. */
function keysPart() {
  return h("div", { class: "keys", "data-field": "/keys" },
    h("p", { class: "muted" }, "Click a key, or click the keyboard and press one. A legend in colour is the file's own; a grey one is the JIS table's, which a slot left out falls back to."),
    h("div", { class: "kb", id: "kb", tabindex: 0, role: "group", "aria-label": "The 48 typing keys and Space",
      onkeydown: e => { if (L.ROWS.flat().includes(e.code) && !e.metaKey && !e.ctrlKey && e.target.id === "kb") { e.preventDefault(); ui.key = e.code; drawKeys(); } } }),
    h("div", { class: "keypanel", id: "keypanel" }),
    h("p", { class: "said", "data-said": "/keys", role: "alert" }));
}

function drawKeys() {
  const kb = $("kb"), panel = $("keypanel");
  if (!kb) return;
  const keys = isObject(doc.keys) ? doc.keys : {};
  const cap = code => {
    const legend = layer => { const { slot, from } = L.slotOf(doc, code, layer); return slot && typeof slot.t === "string" ? h("span", { class: `lg ${layer} ${from === "file" ? "c-" + slot.r : "jis"}` }, slot.t) : from === "file" ? h("span", { class: `lg ${layer} none` }, "∅") : null; };
    const says = L.LAYERS.map(layer => { const { slot, from } = L.slotOf(doc, code, layer); return from === "none" ? null : `${layer} ${slot ? JSON.stringify(slot.t) : "nothing"}${from === "jis" ? " (JIS)" : ""}`; }).filter(Boolean).join(", ");
    return h("button", { class: "cap" + (code === ui.key ? " on" : "") + (isObject(keys[code]) ? " own" : ""), "data-code": code, "aria-pressed": String(code === ui.key), "aria-label": `${code}: ${says || "not the layout's"}`, title: code,
      onclick: () => { ui.key = code; drawKeys(); } },
      code === "Space" && !isObject(keys.Space) ? h("span", { class: "lg jis" }, "Space") : null, L.LAYERS.map(legend));
  };
  kb.replaceChildren(...L.ROWS.map((row, i) => h("div", { class: `kbrow r${i}` }, row.map(cap))));

  const code = ui.key, key = isObject(keys[code]) ? keys[code] : {};
  const roles = L.ROLES_OF[doc.read] ?? Object.keys(L.ROLES);
  const node = L.schemaAt(schema, doc, ["keys", code]) ?? {};
  const layerRow = layer => {
    const path = ["keys", code, layer], p = L.pointer(path), has = Object.hasOwn(key, layer), slot = key[layer];
    const fallback = L.slotOf({ ...doc, keys: {} }, code, layer);
    const state = !has ? "out" : slot === null ? "null" : "own";
    const outLabel = fallback.from === "jis" ? (fallback.slot ? `Left out: the JIS table's ${JSON.stringify(fallback.slot.t)}` : "Left out: the JIS table has nothing here") : "Left out: not the layout's";
    const setState = to => reshape(() => {
      const keysNow = isObject(doc.keys) ? doc.keys : (doc.keys = {});
      const k = isObject(keysNow[code]) ? keysNow[code] : (keysNow[code] = {});
      if (to === "out") { delete k[layer]; if (!Object.keys(k).length) delete keysNow[code]; }
      else if (to === "null") k[layer] = null;
      else k[layer] = isObject(slot) ? slot : fallback.slot && roles.includes(fallback.slot.r) ? { ...fallback.slot } : { t: fallback.slot?.t ?? "", r: "x" };
    }, `${p}: ${to === "out" ? "left out" : to === "null" ? "types nothing" : "its own slot"}.`);
    const own = isObject(slot);
    return h("div", { class: "layer", "data-field": p },
      h("div", { class: "head" }, h("span", { class: "name" }, L.LAYER_NAMES[layer]), h("code", {}, layer)),
      h("div", { class: "row" },
        h("select", { "data-state": p, "aria-label": `What ${p} is`, onchange: e => setState(e.target.value) },
          h("option", { value: "out", selected: state === "out" }, outLabel),
          h("option", { value: "null", selected: state === "null" }, "null: the key types nothing here"),
          h("option", { value: "own", selected: state === "own" }, "A slot of its own")),
        own ? [
          h("label", {}, "types ", h("input", { ...control([...path, "t"], { "aria-label": `What ${p} types` }), type: "text", class: "slot", size: 6, spellcheck: "false", autocomplete: "off", value: typeof slot.t === "string" ? slot.t : "", oninput: e => { put([...path, "t"], e.target.value); drawCaps(); } })),
          h("label", {}, "as ", h("select", { ...control([...path, "r"], { "aria-label": `The role of ${p}` }), onchange: e => { put([...path, "r"], e.target.value); drawCaps(); } },
            roles.includes(slot.r) ? null : h("option", { value: slot.r, selected: true }, `${slot.r} (not a role of ${doc.read})`),
            roles.map(r => h("option", { value: r, selected: r === slot.r }, L.ROLES[r])))),
        ] : null),
      h("p", { class: "about" }, node.properties?.[layer]?.description ?? ""),
      h("p", { class: "said", "data-said": p, role: "alert" }),
      own ? [h("span", { hidden: true, "data-said": p + "/t" }), h("span", { hidden: true, "data-said": p + "/r" })] : null);
  };
  const slotNode = L.deref(schema, L.deref(schema, schema.$defs.slot_any).oneOf[1]);
  panel.replaceChildren(
    h("div", { class: "head" }, h("b", {}, LEGEND[code] ?? code), h("span", { class: "muted mono" }, `/keys/${code}`),
      isObject(keys[code]) ? h("button", { class: "quiet", "data-remove": `/keys/${code}`, "aria-label": `Remove /keys/${code}`, onclick: () => reshape(() => { delete doc.keys[code]; }, `/keys/${code}: every layer left out.`) }, "Leave every layer out") : null),
    ...L.LAYERS.map(layerRow),
    h("div", { class: "about" }, h("p", {}, h("b", {}, "t"), ": ", slotNode.properties.t.description), h("p", {}, h("b", {}, "r"), ": ", slotNode.properties.r.description),
      h("p", { class: "said", "data-said": `/keys/${code}`, role: "alert" })));
}
/** The keyboard alone, while a slot is typed in: the panel keeps its caret. */
function drawCaps() {
  const panel = $("keypanel"), kept = [...panel.childNodes];
  drawKeys();
  panel.replaceChildren(...kept);
}

// ── The page ───────────────────────────────────────────────────────────────────────────────────

const SECTIONS = [
  ["basics", "The layout", ["format", "id", "name", "lang", "kind", "read", "note"]],
  ["keys", "Keys", ["keys"]],
  ["reading", "How it reads", ["direct", "hangul", "table", "kana", "search", "text_ends_word"]],
  ["rows", "Rows of the menu", ["convert", "forms", "words", "rank"]],
  ["more", "Actions, particles, options", ["actions", "particles", "options"]],
];

function draw() {
  const root = $("form"), y = scrollY;
  const placed = new Set(SECTIONS.flatMap(s => s[2]));
  const rest = Object.keys(schema.properties).filter(k => !placed.has(k));
  const sections = rest.length ? [...SECTIONS, ["other", "Other", rest]] : SECTIONS;
  const required = schema.required ?? [];
  const one = name => {
    const path = [name], node = L.schemaAt(schema, doc, path) ?? L.deref(schema, schema.properties[name]);
    const has = Object.hasOwn(doc, name);
    // A part of another way of reading shows only when the file has it, to be removed.
    if (L.PARTS.includes(name) && name !== doc.read && !has) return null;
    if (name === "keys" && has && isObject(doc.keys)) return h("div", { class: "field", "data-field": "/keys-about" }, h("div", { class: "head" }, h("span", { class: "name" }, "Keys"), h("code", {}, "keys")), keysPart(), h("div", { class: "about" }, h("p", {}, node.description), h("p", { class: "schema" }, h("span", { class: "mono" }, "/keys"), " · ", L.limits(schema, node), " · required")));
    if (name === "read" && has) return readField(node);
    if (!has) return absent(path, node, required.includes(name) || name === doc.read);
    return field(path, node, doc[name], required.includes(name) || name === doc.read);
  };
  root.replaceChildren(
    ...sections.map(([id, title, names]) => h("section", { id, "aria-labelledby": id + "-h" }, h("h2", { id: id + "-h" }, title), names.map(one))),
    Object.keys(doc).filter(k => !Object.hasOwn(schema.properties, k)).map(k => unknown([k], doc[k])));
  drawKeys();
  $("nav").replaceChildren(...[...sections, ["json", "JSON"], ["try", "Try it"], ["agents", "For a program"]].map(([id, title]) => h("a", { href: "#" + id }, title)));
  scrollTo(0, y);
}

/** `read` changes more than itself: the kind, the part that holds its tables, and the roles a slot can have. */
function readField(node) {
  const f = field(["read"], node, doc.read, true);
  const select = h("select", { ...control(["read"], { "aria-label": "Way of reading" }), id: "f:/read", onchange: e => {
    const to = e.target.value;
    let dropped = [];
    reshape(() => { dropped = L.setRead(schema, doc, to); });
    ui.note = `Reads as ${to}.` + (dropped.length ? ` Changed with it: ${dropped.slice(0, 6).join(", ")}${dropped.length > 6 ? ` and ${dropped.length - 6} more` : ""}. Undo brings them back.` : "");
    say();
  } }, L.READS.includes(doc.read) ? null : h("option", { selected: true }, String(doc.read)), L.READS.map(r => h("option", { value: r, selected: r === doc.read }, r)));
  f.querySelector("[data-path]").replaceWith(select);
  f.querySelector(".about").prepend(h("p", { class: "muted" }, "Changing it here also sets kind, adds this way's part, removes the other ways' parts, and gives every slot a role this way has."));
  return f;
}

/** The file as text, in the JSON box and in the page for a program to read. */
function mirror() {
  const text = L.print(doc);
  $("bada-layout").textContent = text;
  if (document.activeElement !== $("jsonText")) { $("jsonText").value = text; $("jsonSaid").textContent = ""; }
  document.title = `${doc.id ?? "layout"} · bada layout editor`;
  fillStart();
}

function fillStart() {
  const mine = saved(), select = $("start");
  const group = (label, options) => h("optgroup", { label }, options.map(([v, text]) => h("option", { value: v }, text)));
  select.replaceChildren(h("option", { value: "" }, "Start from…"),
    group("A stock layout", Object.values(stock).map(l => [`stock:${l.id}`, `${l.name?.en ?? l.id}  (${l.id}, ${l.read})`])),
    Object.keys(mine).length ? group("Saved in this browser", Object.values(mine).map(l => [`mine:${l.id}`, `${l.name?.en ?? l.id}  (${l.id})`])) : null,
    group("A new layout", L.READS.map(r => [`new:${r}`, `Empty, read as ${r}`])));
}
function start(from) {
  const [kind, name] = from.split(":");
  if (kind === "stock" && stock[name]) return load(stock[name], `Started from the stock layout ${name}. Give it an id of your own, or keep the id to replace the stock one.`);
  if (kind === "mine" && saved()[name]) return load(saved()[name], `Opened ${name}, saved in this browser.`);
  if (kind === "new" && L.READS.includes(name)) return load(L.fresh(schema, name), `A new layout that reads as ${name}.`);
  return false;
}
function take(text, where) {
  const got = L.read(text);
  if (got.error) return got.error;
  load(got.value, `Loaded ${where}.`);
  return null;
}

function save() {
  check(); ask();
  if (told.length || errors().length) return false;
  storage.setItem(STORE.layouts, JSON.stringify({ ...saved(), [doc.id]: doc }));
  ui.note = `Saved ${doc.id} in this browser. The typing page has it at its next load: add it to a profile in its Settings.`;
  say(); fillStart();
  return true;
}
function removeSaved() {
  const mine = saved();
  if (!mine[doc.id]) return false;
  delete mine[doc.id];
  if (Object.keys(mine).length) storage.setItem(STORE.layouts, JSON.stringify(mine)); else storage.removeItem(STORE.layouts);
  ui.note = `Removed ${doc.id} from this browser. The draft is still here.`;
  say(); fillStart();
  return true;
}
function download() {
  const a = h("a", { href: URL.createObjectURL(new Blob([L.print(doc)], { type: "application/json" })), download: `${doc.id || "layout"}.json` });
  a.click();
  URL.revokeObjectURL(a.href);
}

// ── Trying it ──────────────────────────────────────────────────────────────────────────────────

const tried = { field: new Field(), handle: null };

/** Gives the engine's session the draft, and puts the try-it field in it: muhenkan for a layout that types directly, a profile of it alone otherwise. */
function feed() {
  const box = $("tryBox");
  if (!engine || told.length || errors().length) { box.classList.add("off"); $("trySaid").textContent = "The draft has errors: the field types with the last draft that had none."; return; }
  if (fed && fed !== doc.id) engine.send(null, { type: "layout", id: fed, value: null });
  if (!engine.send(null, { type: "layout", value: doc }).ok) return;
  fed = doc.id;
  const direct = doc.kind === "muhenkan";
  engine.send(null, { type: "settings", set: { muhenkan: { layout: direct ? doc.id : null }, henkan: { profiles: direct ? [] : [{ id: "try", name: "Try", layouts: [doc.id] }] } } });
  tried.handle ??= engine.field();
  const reply = engine.send(tried.handle, { type: "select", mode: direct ? "muhenkan" : "henkan", profile: direct ? null : "try" });
  tried.field.apply(reply);
  box.classList.remove("off");
  $("trySaid").textContent = direct ? "In muhenkan with this layout." : "In henkan, in a profile of this layout alone. This page gives the engine no data, so nothing is converted and no word is known.";
  drawTry();
}
function tryKey(e) {
  if (!tried.handle || e.metaKey || e.ctrlKey || e.isComposing) return;
  if (e.key === "Tab" && !tried.field.marked) return;
  e.preventDefault();
  const f = tried.field, send = event => engine.send(tried.handle, event);
  const reply = send({ type: "key", key: e.code, shift: e.shiftKey, ctrl: false, alt: e.altKey, meta: false, ...(e.repeat ? { repeat: true } : {}) });
  f.apply(reply, send);
  if (reply.ok && !reply.consumed) {
    if (typeof reply.types === "string") f.insert(reply.types);
    else if (e.code === "Space") f.insert(" ");
    else if (e.code === "Enter") f.insert("\n");
    else f.key(e.code, { shift: e.shiftKey });
  }
  drawTry();
}
function drawTry() {
  const f = tried.field, m = f.marked, text = f.text;
  const caret = h("span", { class: "caret" });
  const parts = m ? [text.slice(0, m.start), h("span", { class: "marked" }, text.slice(m.start, m.start + m.len)), caret, text.slice(m.start + m.len)] : [text.slice(0, f.end), caret, text.slice(f.end)];
  $("tryText").replaceChildren(...parts);
  $("tryMenu").replaceChildren(...f.menu.rows.map((r, i) => h("li", { class: i === f.menu.selected ? "sel" : "", onclick: () => { const send = e => engine.send(tried.handle, e); f.apply(send({ type: "pick", row: i }), send); f.apply(send({ type: "flush", cause: "harness" }), send); drawTry(); $("tryBox").focus(); } },
    r.text, r.note ? h("small", {}, " ", r.note) : null)));
}

// ── For a program ──────────────────────────────────────────────────────────────────────────────

/**
 * `window.badaLayoutEditor`: the editor without its controls. Paths are JSON pointers into the
 * file ("/rank/known", "/keys/KeyA/base", "/convert/0/rows"; "" is the whole file). Every call
 * that changes the draft returns `status()`.
 */
const api = {
  /** The layout schema (JSON Schema 2020-12), with a description of every field. */
  get schema() { return schema; },
  /** The draft, or the value at a pointer. A copy. */
  get: (path = "") => L.copy(L.get(doc, L.parse(path))),
  /** The draft as the file's text. */
  text: () => L.print(doc),
  /** Sets the value at a pointer, making the objects on the way. "/list/-" appends to a list. */
  set(path, value) { reshape(() => L.set(doc, L.parse(path), L.copy(value)), `Set ${path}.`); return api.status(); },
  /** Removes a field, an entry or a list item. */
  remove(path) { reshape(() => L.remove(doc, L.parse(path)), `Removed ${path}.`); return api.status(); },
  /** Changes the way of reading, with all that follows from it (kind, the way's part, the slots' roles). */
  setRead(read) { reshape(() => L.setRead(schema, doc, read), `Reads as ${read}.`); return api.status(); },
  /** Replaces the draft: a layout object, or a file's text. */
  load(value) { const got = typeof value === "string" ? L.read(value) : { value }; if (got.error) throw new Error(got.error); load(got.value, "Loaded."); return api.status(); },
  /** Starts from "stock:<id>", "mine:<id>" (saved in this browser) or "new:<read>". */
  start(from) { if (start(from) === false) throw new Error(`nothing to start from: ${from}`); return api.status(); },
  /** What can be started from. */
  sources: () => [...Object.keys(stock).map(id => `stock:${id}`), ...Object.keys(saved()).map(id => `mine:${id}`), ...L.READS.map(r => `new:${r}`)],
  /** One place of the file: its title, description, limits, schema, whether it is required and present, and its value. Works for a place the file doesn't have yet. Null where the format has none. */
  describe: (path = "") => L.describe(schema, doc, path),
  /** The fields a part can have, each described: describe() of every property of the object at `path`. */
  fields(path = "") {
    const node = L.schemaAt(schema, doc, L.parse(path));
    const form = node?.oneOf ? L.deref(schema, node.oneOf[node.variant ?? 0]) : node;
    return Object.keys(form?.properties ?? {}).map(k => L.describe(schema, doc, `${path}/${k}`));
  },
  /** Every pointer the draft has a value at. */
  places: () => L.places(doc),
  /** The least value that fits the schema at a pointer: what "+ Add" would put there. */
  blank(path) { const node = L.schemaAt(schema, doc, L.parse(path)); return node ? L.blank(schema, node) : undefined; },
  /** Checks the draft now. `format`: the schema's problems. `engine`: the engine's, each with a level. `ok`: no error of either. */
  status() {
    told = L.validate(schema, doc);
    if (engine) { clearTimeout(checking); ask(); }
    return { id: doc.id, ok: !told.length && !errors().length && !!engine, format: L.copy(told), engine: L.copy(heard), saved: Object.hasOwn(saved(), doc.id), stock: Object.hasOwn(stock, doc.id) };
  },
  /** Saves the draft in this browser, where the typing page finds it. Refused while there is an error. */
  save: () => ({ saved: save(), ...api.status() }),
  /** Takes the saved copy out of this browser. The draft stays. */
  unsave: () => ({ removed: removeSaved(), ...api.status() }),
  undo: () => ({ undone: undo(), ...api.status() }),
  /** Scrolls to the control of a pointer and focuses it. */
  reveal,
  /** Types key codes ("KeyA", "shift+KeyA", "Space", "Enter") into the try-it field. Returns its text, the marked part and the menu's rows. */
  type(...chords) {
    if (!tried.handle) api.status();
    for (const chord of chords.flat()) {
      const parts = chord.split("+"), code = parts.pop();
      tryKey({ code, key: code, shiftKey: parts.includes("shift"), altKey: parts.includes("alt"), preventDefault() {} });
    }
    const f = tried.field;
    return { text: f.text, marked: f.markedText, rows: f.menu.rows.map(r => r.text) };
  },
  /** Empties the try-it field. */
  clear() { if (tried.handle) tried.field.apply(engine.send(tried.handle, { type: "reset" })); tried.field.set("", 0, 0); drawTry(); },
};

// ── Start ──────────────────────────────────────────────────────────────────────────────────────

async function main() {
  schema = await (await fetch("layout.schema.json", { cache: "no-cache" })).json();
  // The schema and a draft are enough to edit; the engine comes when it has loaded.
  const kept = L.read(storage.getItem(DRAFT) ?? "");
  doc = kept.value ?? L.fresh(schema);
  ui.note = kept.value ? "This is the draft you left here." : "A new layout. Start from a stock layout or load a file to edit one that exists.";

  $("start").onchange = e => { if (e.target.value) start(e.target.value); e.target.value = ""; };
  $("file").onchange = async e => { const f = e.target.files[0]; if (f) { const error = take(await f.text(), f.name); if (error) { ui.note = `${f.name}: ${error}`; say(); } } e.target.value = ""; };
  $("open").onclick = () => $("file").click();
  $("save").onclick = save;
  $("remove").onclick = removeSaved;
  $("undo").onclick = undo;
  $("download").onclick = download;
  $("copy").onclick = async () => { try { await navigator.clipboard.writeText(L.print(doc)); ui.note = "Copied the JSON."; } catch { ui.note = "The browser didn't let the page copy: select the text in the JSON box."; } say(); };
  $("jsonApply").onclick = () => { const error = take($("jsonText").value, "the JSON box"); $("jsonSaid").textContent = error ?? ""; };
  $("jsonText").oninput = () => { const got = L.read($("jsonText").value); $("jsonSaid").textContent = got.error ?? "Not applied yet."; };
  $("tryBox").onkeydown = tryKey;
  $("tryClear").onclick = () => { api.clear(); $("tryBox").focus(); };
  window.badaLayoutEditor = api;

  draw(); mirror(); check();
  const loaded = await loadEngine((got, total) => { $("status").textContent = `Loading the engine: ${Math.round((100 * got) / total)}%`; });
  engine = loaded.engine;
  stock = loaded.layouts;
  fillStart();
  // layout.html?from=stock:ja-kana starts from a layout by its address.
  const from = new URLSearchParams(location.search).get("from");
  if (from && start(from) !== false) history.replaceState(null, "", location.pathname);
  check();
  document.documentElement.dataset.ready = "true";
}

main().catch(e => { $("status").className = "status bad"; $("status").textContent = `The editor didn't start: ${e.message}`; });
