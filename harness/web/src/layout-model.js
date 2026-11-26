// The layout editor's model (layout-editor.js is its page): a bada-layout/3 file as an object, read
// and changed through the layout schema (docs/schemas/layout.schema.json), which the build copies
// beside the page. The schema says what every field is, so the editor has no list of its own: a
// field added to the schema shows in the editor. No DOM here.

import { JIS } from "./keyboards.js";

export const FORMAT = "bada-layout/3";
export const READS = ["direct", "hangul", "table", "kana", "search"];
export const LAYERS = ["base", "shift", "alt", "alt_shift"];
export const LAYER_NAMES = { base: "No modifier", shift: "⇧ Shift", alt: "⌥ Option / Alt", alt_shift: "⌥⇧ Option + Shift" };
/** The typing keys, row by row, as a JIS keyboard has them; then Space. */
export const ROWS = [
  ["Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8", "Digit9", "Digit0", "Minus", "Equal", "IntlYen"],
  ["KeyQ", "KeyW", "KeyE", "KeyR", "KeyT", "KeyY", "KeyU", "KeyI", "KeyO", "KeyP", "BracketLeft", "BracketRight"],
  ["KeyA", "KeyS", "KeyD", "KeyF", "KeyG", "KeyH", "KeyJ", "KeyK", "KeyL", "Semicolon", "Quote", "Backslash"],
  ["KeyZ", "KeyX", "KeyC", "KeyV", "KeyB", "KeyN", "KeyM", "Comma", "Period", "Slash", "IntlRo"],
  ["Space"],
];
/** A slot's roles (the standard, §7.5.1), and which a way of reading has. */
export const ROLES = {
  x: "x: text, typed as it is",
  l: "l: a letter, of a word, a spelling or the search",
  c: "c: a consonant, initial and final",
  i: "i: an initial consonant only",
  f: "f: a final consonant only",
  v: "v: a vowel",
  b: "b: a break between syllables",
  k: "k: a kana",
  m: "m: a mark on the kana before it",
};
export const ROLES_OF = { direct: ["l", "x"], hangul: ["c", "i", "f", "v", "x"], table: ["l", "b", "x"], kana: ["k", "m", "x"], search: ["l", "x"] };
/** The parts of the file that belong to one way of reading. */
export const PARTS = READS;

const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);
const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
export const copy = v => (v === undefined ? undefined : JSON.parse(JSON.stringify(v)));

// ── JSON pointers ──────────────────────────────────────────────────────────────────────────────

/** A path (`["keys", "KeyA", "base"]`) as a JSON pointer (`/keys/KeyA/base`). The whole file is "". */
export const pointer = path => path.map(p => "/" + String(p).replace(/~/g, "~0").replace(/\//g, "~1")).join("");
/** A JSON pointer as a path. Array indexes stay strings. */
export function parse(text) {
  if (text === "" || text == null) return [];
  if (!text.startsWith("/")) throw new Error(`a JSON pointer starts with "/": ${text}`);
  return text.slice(1).split("/").map(p => p.replace(/~1/g, "/").replace(/~0/g, "~"));
}
export function get(doc, path) {
  let at = doc;
  for (const p of path) {
    if (at === null || typeof at !== "object") return undefined;
    at = at[p];
  }
  return at;
}
/** Sets the value at `path`, making the objects on the way. An array index one past the end appends. */
export function set(doc, path, value) {
  if (!path.length) throw new Error("the whole file is replaced with load, not set");
  let at = doc;
  path.slice(0, -1).forEach((p, i) => {
    if (at[p] === null || typeof at[p] !== "object") at[p] = /^\d+$/.test(path[i + 1]) ? [] : {};
    at = at[p];
  });
  const last = path[path.length - 1];
  if (Array.isArray(at)) {
    const index = last === "-" ? at.length : Number(last);
    if (!Number.isInteger(index) || index < 0 || index > at.length) throw new Error(`no place ${last} in a list of ${at.length}`);
    at[index] = value;
  } else at[last] = value;
}
/** Removes what is at `path`: a property, or an item of a list. Returns whether it was there. */
export function remove(doc, path) {
  const parent = get(doc, path.slice(0, -1)), last = path[path.length - 1];
  if (Array.isArray(parent)) {
    if (!(Number(last) in parent)) return false;
    parent.splice(Number(last), 1);
    return true;
  }
  if (!isObject(parent) || !own(parent, last)) return false;
  delete parent[last];
  return true;
}

// ── The schema ─────────────────────────────────────────────────────────────────────────────────

/** A schema node with its `$ref` followed. The reference's own title and description win. */
export function deref(root, node) {
  let seen = 0;
  while (isObject(node) && typeof node.$ref === "string") {
    const { $ref, ...mine } = node;
    const target = get(root, parse($ref.slice(1)));
    if (!isObject(target) || seen++ > 20) throw new Error(`the schema has no ${$ref}`);
    node = { ...target, ...mine };
  }
  return node;
}

const kindOf = v => (v === null ? "null" : Array.isArray(v) ? "array" : typeof v === "number" && Number.isInteger(v) ? "integer" : typeof v);
const isType = (v, t) => { const k = kindOf(v); return k === t || (t === "number" && k === "integer"); };
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

/** What is wrong with `value` by `node`: `[{path, message}]`, where `path` is a JSON pointer. */
function problemsOf(root, node, value, path, out) {
  if (node === true) return out;
  if (node === false) { out.push({ path: pointer(path), message: "isn't part of this kind of layout" }); return out; }
  node = deref(root, node);
  const say = message => out.push({ path: pointer(path), message });
  if (own(node, "const") && !same(value, node.const)) say(`must be ${JSON.stringify(node.const)}`);
  if (node.enum && !node.enum.some(e => same(e, value))) say(`must be one of ${node.enum.map(e => JSON.stringify(e)).join(", ")}`);
  if (node.type && !isType(value, node.type)) { say(`must be ${article(node.type)}, and is ${article(kindOf(value))}`); return out; }
  if (typeof value === "string") {
    const length = [...value].length;
    if (node.minLength != null && length < node.minLength) say(length ? `is shorter than ${node.minLength} characters` : "is empty");
    if (node.maxLength != null && length > node.maxLength) say(`is longer than ${node.maxLength} characters`);
    if (node.pattern && !new RegExp(node.pattern, "u").test(value)) say(`doesn't match ${node.pattern}`);
  }
  if (typeof value === "number") {
    if (node.minimum != null && value < node.minimum) say(`is under ${node.minimum}`);
    if (node.maximum != null && value > node.maximum) say(`is over ${node.maximum}`);
  }
  if (Array.isArray(value)) {
    if (node.minItems != null && value.length < node.minItems) say(`needs ${node.minItems} item${node.minItems > 1 ? "s" : ""} at least`);
    if (node.uniqueItems && new Set(value.map(v => JSON.stringify(v))).size !== value.length) say("lists an item twice");
    if (node.items !== undefined) value.forEach((item, i) => problemsOf(root, node.items, item, [...path, i], out));
  }
  if (isObject(value)) {
    const keys = Object.keys(value);
    if (node.minProperties != null && keys.length < node.minProperties) say(keys.length ? `needs ${node.minProperties} entries at least` : "is empty");
    for (const name of node.required ?? []) if (!own(value, name)) out.push({ path: pointer([...path, name]), message: "is missing" });
    for (const key of keys) {
      if (node.propertyNames !== undefined && problemsOf(root, node.propertyNames, key, [], []).length) out.push({ path: pointer([...path, key]), message: "isn't a name this part can have" });
      if (node.properties && own(node.properties, key)) problemsOf(root, node.properties[key], value[key], [...path, key], out);
      else if (node.additionalProperties === false) out.push({ path: pointer([...path, key]), message: "isn't a field of the format" });
      else if (isObject(node.additionalProperties)) problemsOf(root, node.additionalProperties, value[key], [...path, key], out);
    }
  }
  if (node.oneOf) {
    const fits = node.oneOf.filter(branch => !problemsOf(root, branch, value, path, []).length).length;
    if (fits !== 1) {
      const at = variantOf(root, node, value);
      const inner = problemsOf(root, node.oneOf[at], value, path, []);
      if (inner.length) out.push(...inner);
      else say("fits more than one of its forms");
    }
  }
  for (const part of node.allOf ?? []) problemsOf(root, part, value, path, out);
  if (node.if !== undefined && node.then !== undefined && !problemsOf(root, node.if, value, path, []).length) problemsOf(root, node.then, value, path, out);
  return out;
}
const article = type => (/^[aeio]/.test(type) ? "an " : "a ") + type;

/** What is wrong with a layout file by the schema: `[{path, message}]`, none when it fits. */
export function validate(root, doc) {
  const out = problemsOf(root, root, doc, [], []);
  const seen = new Set();
  return out.filter(p => { const key = p.path + "\n" + p.message; return !seen.has(key) && seen.add(key); });
}

/**
 * Which of a `oneOf`'s forms `value` is, by index: the one whose constant fields it has (`with`,
 * `do`, `type`), else the one whose required fields it has, else the one it fits, else the first.
 */
export function variantOf(root, node, value) {
  const branches = node.oneOf.map(b => deref(root, b));
  const constants = b => Object.entries(b.properties ?? {}).filter(([, p]) => isObject(p) && own(p, "const"));
  if (value === null) { const at = branches.findIndex(b => b.type === "null"); if (at >= 0) return at; }
  if (isObject(value)) {
    let at = branches.findIndex(b => constants(b).length && constants(b).every(([k, p]) => same(value[k], p.const)));
    if (at >= 0) return at;
    at = branches.findIndex(b => b.type === "object" && !constants(b).length && (b.required ?? []).length && b.required.every(k => own(value, k)));
    if (at >= 0) return at;
  }
  const at = node.oneOf.findIndex(b => !problemsOf(root, b, value, [], []).length);
  return at >= 0 ? at : Math.max(0, branches.findIndex(b => b.type !== "null"));
}

/** The least value that fits `node`: its required fields, each the least that fits. */
export function blank(root, node) {
  node = deref(root, node);
  if (own(node, "const")) return copy(node.const);
  if (own(node, "default")) return copy(node.default);
  if (node.enum) return copy(node.enum[0]);
  if (node.oneOf) return blank(root, node.oneOf[Math.max(0, node.oneOf.findIndex(b => deref(root, b).type !== "null"))]);
  switch (node.type) {
    case "object": {
      const value = Object.fromEntries((node.required ?? []).map(k => [k, blank(root, node.properties?.[k] ?? node.additionalProperties ?? {})]));
      // A map that needs an entry gets a placeholder the user renames.
      if (node.minProperties && !Object.keys(value).length && isObject(node.additionalProperties)) value.a = blank(root, node.additionalProperties);
      return value;
    }
    case "array": return Array.from({ length: node.minItems ?? 0 }, () => blank(root, node.items ?? {}));
    case "string": {
      // A name that must have a shape gets one that has it.
      const fits = text => !problemsOf(root, node, text, [], []).length;
      return ["", "a", "en", "en-mine"].find(fits) ?? "";
    }
    case "integer": case "number": return node.minimum ?? 0;
    case "boolean": return false;
    case "null": return null;
  }
  return null;
}

/** The keys schema a way of reading has (its slots' roles). */
function keysOf(root, read) {
  const then = (root.allOf ?? []).find(part => part.if?.properties?.read?.const === read)?.then;
  const { title, description } = root.properties.keys;
  return { ...deref(root, then?.properties?.keys ?? root.properties.keys), title, description };
}

/**
 * The schema of what is, or could be, at `path` in `doc`: followed through `$ref`, with the form
 * of each `oneOf` on the way chosen by what the file holds there. Null where the format has no
 * such place. The last node is returned as it is written when it is a `oneOf`, with `variant`,
 * the index of the form in use.
 */
export function schemaAt(root, doc, path) {
  let node = deref(root, root), value = doc;
  const choose = () => {
    while (node.oneOf) node = deref(root, node.oneOf[variantOf(root, node, value)]);
  };
  for (let i = 0; i < path.length; i++) {
    choose();
    const p = String(path[i]);
    let next;
    if (i === 0 && p === "keys") next = keysOf(root, doc?.read);
    else if (Array.isArray(value) || node.type === "array") next = node.items;
    else if (node.properties && own(node.properties, p)) next = node.properties[p];
    else if (isObject(node.additionalProperties)) next = node.additionalProperties;
    if (next === undefined || next === false) return null;
    node = deref(root, next);
    value = value === null || typeof value !== "object" ? undefined : value[p];
  }
  if (node.oneOf) return { ...node, variant: value === undefined ? null : variantOf(root, node, value) };
  return node;
}

/** A schema node's limits in a line: `integer, 1 or more`, `one of "half", "full"`. */
export function limits(root, node) {
  node = deref(root, node);
  const out = [];
  if (own(node, "const")) return `always ${JSON.stringify(node.const)}`;
  if (node.enum) return `one of ${node.enum.map(e => JSON.stringify(e)).join(", ")}`;
  if (node.oneOf) return `one of ${node.oneOf.length} forms: ${node.oneOf.map(b => { const d = deref(root, b); return d.title ?? (d.type === "null" ? "null" : d.type); }).join(", ")}`;
  if (node.type) out.push(node.type === "object" && isObject(node.additionalProperties) && !node.properties ? "object (a map)" : node.type === "array" ? "array (a list)" : node.type);
  if (node.pattern) out.push(`matches ${node.pattern}`);
  if (node.minLength) out.push(node.minLength === 1 ? "not empty" : `${node.minLength} characters or more`);
  if (node.maxLength != null) out.push(`${node.maxLength} characters at most`);
  if (node.minimum != null) out.push(`${node.minimum} or more`);
  if (node.maximum != null) out.push(`${node.maximum} at most`);
  if (node.minItems) out.push(`${node.minItems} item${node.minItems > 1 ? "s" : ""} or more`);
  if (node.uniqueItems) out.push("no item twice");
  if (node.minProperties) out.push(`${node.minProperties} entr${node.minProperties > 1 ? "ies" : "y"} or more`);
  if (node.type === "array" && node.items) out.push(`each: ${limits(root, node.items)}`);
  if (node.type === "object" && isObject(node.additionalProperties) && !node.properties) out.push(`each value: ${limits(root, node.additionalProperties)}`);
  return out.join(", ");
}

/**
 * Everything about one place of the file, for a person or a program: its name, what it does, its
 * schema, and what it holds now.
 */
export function describe(root, doc, text) {
  const path = parse(text), node = schemaAt(root, doc, path);
  if (!node) return null;
  const parent = path.length ? schemaAt(root, doc, path.slice(0, -1)) : null;
  const inForm = parent?.oneOf ? deref(root, parent.oneOf[parent.variant ?? 0]) : parent;
  const { variant, ...schema } = node;
  return {
    path: text,
    title: node.title ?? (path.length ? String(path[path.length - 1]) : root.title),
    description: node.description ?? "",
    limits: limits(root, node),
    required: !!inForm?.required?.includes(String(path[path.length - 1])),
    present: get(doc, path) !== undefined,
    value: copy(get(doc, path)),
    ...(variant != null ? { form: variant } : {}),
    schema,
  };
}

// ── A layout ───────────────────────────────────────────────────────────────────────────────────

/** A key's slot on a layer as the engine reads it: the file's, else the JIS table's (§7.5.1), else none. */
export function slotOf(doc, code, layer) {
  const key = doc.keys?.[code];
  if (isObject(key) && own(key, layer)) return { slot: key[layer], from: "file" };
  if (code === "Space" || layer === "alt" || layer === "alt_shift") return { slot: null, from: "none" };
  const legend = JIS[code]?.[layer === "base" ? 0 : 1];
  if (!legend) return { slot: null, from: "jis" };
  const letters = ["direct", "table", "search"].includes(doc.read);
  return { slot: { t: legend, r: code.startsWith("Key") && letters ? "l" : "x" }, from: "jis" };
}

/** A new layout that reads the keys one way: the least file the format takes. */
export function fresh(root, read = "direct", id = "en-mine") {
  const doc = { format: FORMAT, id, name: { en: "My layout" }, lang: id.split("-")[0] === "uni" ? "und" : id.split("-")[0], kind: read === "direct" ? "muhenkan" : "henkan", read, keys: {} };
  if (read !== "direct") doc[read] = blank(root, root.properties[read]);
  return doc;
}

/**
 * Changes how `doc` reads its keys: `kind` follows, the new way's part is added if it isn't there,
 * the other ways' parts go, and a slot whose role the new way doesn't have becomes a letter where
 * it has letters and text otherwise. Returns what was dropped, as pointers.
 */
export function setRead(root, doc, read) {
  const dropped = [];
  doc.read = read;
  doc.kind = read === "direct" ? "muhenkan" : "henkan";
  for (const part of PARTS) {
    if (part !== read && own(doc, part)) { delete doc[part]; dropped.push("/" + part); }
  }
  if (read !== "direct" && !own(doc, read)) doc[read] = blank(root, root.properties[read]);
  const roles = ROLES_OF[read];
  for (const [code, key] of Object.entries(isObject(doc.keys) ? doc.keys : {})) {
    for (const [layer, slot] of Object.entries(isObject(key) ? key : {})) {
      if (isObject(slot) && !roles.includes(slot.r)) {
        slot.r = roles.includes("l") && [...String(slot.t)].length === 1 && /\p{L}/u.test(slot.t) ? "l" : "x";
        dropped.push(pointer(["keys", code, layer, "r"]));
      }
    }
  }
  return dropped;
}

/** The file as it is written: two spaces, a newline at the end. */
export const print = doc => JSON.stringify(doc, null, 2) + "\n";

/** A file's text as a layout object, or why it isn't one. */
export function read(text) {
  let value;
  try { value = JSON.parse(text); } catch (e) { return { error: `This isn't JSON: ${e.message}` }; }
  if (!isObject(value)) return { error: "A layout is a JSON object." };
  return { value };
}

/** Every place of the file that holds something, as pointers: for a program that drives the editor. */
export function places(doc, path = [], out = []) {
  if (path.length) out.push(pointer(path));
  if (doc !== null && typeof doc === "object") for (const key of Object.keys(doc)) places(doc[key], [...path, key], out);
  return out;
}
