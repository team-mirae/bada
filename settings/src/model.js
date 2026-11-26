// The settings file as the page changes it, with no DOM (the standard, §8 to §10): the parts of a
// new install, the presets, what a key and an action are called, what a binding says in words,
// and the changes that touch more than one place. A host checks what is written; nothing here does.

export const FORMAT = "bada-settings/2";
export const LAYOUT_FORMAT = "bada-layout/3";
export const DICTIONARY_FORMAT = "bada-dictionary";

/** The language keys of §9.3, in the order a keyboard has them. */
export const LANGUAGE_KEYS = ["Backquote", "CapsLock", "NonConvert", "Convert", "KanaMode", "Lang2", "Lang1"];
export const KEY_NAMES = {
  Lang1: "かな", Lang2: "英数", Convert: "変換", NonConvert: "無変換", KanaMode: "カタカナ", Backquote: "半角/全角", CapsLock: "Caps Lock",
  Space: "Space", Tab: "Tab", Enter: "Enter", Escape: "Esc", Backspace: "Backspace", ArrowUp: "↑", ArrowDown: "↓", ArrowLeft: "←", ArrowRight: "→",
};
const MODIFIERS = ["ctrl", "alt", "shift", "meta"];
const MODIFIER_NAMES = {
  mac: { ctrl: "⌃", alt: "⌥", shift: "⇧", meta: "⌘", join: "" },
  windows: { ctrl: "Ctrl", alt: "Alt", shift: "Shift", meta: "Win", join: "+" },
  linux: { ctrl: "Ctrl", alt: "Alt", shift: "Shift", meta: "Super", join: "+" },
  web: { ctrl: "Ctrl", alt: "Alt", shift: "Shift", meta: "Meta", join: "+" },
};

/** The presets of §9.5, by keyboard. */
export const PRESETS = {
  "jis-mac": [["Lang2", ["mode:muhenkan"]], ["Lang1", ["profile.first"]], ["Lang1", ["menu.next", "profile.first"], "henkan"],
    ["shift+Lang1", ["menu.prev"], "henkan"], ["ctrl+Lang1", ["fix"], "henkan"], ["Backquote", ["width.full"]], ["shift+Backquote", ["width.half"]]],
  "jis-pc": [["NonConvert", ["mode:muhenkan"]], ["Convert", ["profile.first"]], ["Convert", ["menu.next", "profile.first"], "henkan"],
    ["shift+Convert", ["menu.prev"], "henkan"], ["ctrl+Convert", ["fix"], "henkan"], ["Backquote", ["width.full"]], ["shift+Backquote", ["width.half"]]],
  none: [["CapsLock", ["mode.toggle"]]],
};
export const PRESET_NAMES = { "jis-mac": "JIS on a Mac", "jis-pc": "JIS on a PC", none: "No language keys" };
export const preset = name => (PRESETS[name] ?? PRESETS.none).map(([on, d, when]) => ({ on, do: [...d], ...(when ? { when } : {}) }));

/** The preset a file with no bindings gets on a platform, when the harness's own `keyboard` doesn't say. */
export const platformPreset = platform => (platform === "mac" ? "jis-mac" : "jis-pc");

/** A new install's file (§8.3): no muhenkan layout, no profile, the keyboard's preset. */
export const fresh = () => ({ format: FORMAT, engine: { muhenkan: {}, henkan: { profiles: [] } }, keys: {}, harness: {} });

/** The file's text as it is written: two spaces, a newline at the end. */
export const print = file => JSON.stringify(file, null, 2) + "\n";

/** A file's text read, with the parts the page changes made present. Not JSON, or not an object: null. */
export function read(text) {
  let file;
  try { file = JSON.parse(text); } catch { return null; }
  if (!file || typeof file !== "object" || Array.isArray(file)) return null;
  file.engine = obj(file.engine);
  file.engine.muhenkan = obj(file.engine.muhenkan);
  file.engine.henkan = obj(file.engine.henkan);
  if (!Array.isArray(file.engine.henkan.profiles)) file.engine.henkan.profiles = [];
  file.keys = obj(file.keys);
  file.harness = obj(file.harness);
  return file;
}
const obj = v => (v && typeof v === "object" && !Array.isArray(v) ? v : {});

export const profiles = file => file.engine.henkan.profiles;
export const profileOf = (file, id) => profiles(file).find(p => p.id === id);

/** An id nothing has yet, from a wanted one: `main`, `main-2`, `main-3`. */
export function freeId(taken, wanted) {
  const base = slug(wanted) || "profile";
  let id = base, n = 2;
  while (taken.includes(id)) id = `${base.slice(0, 29)}-${n++}`;
  return id;
}
/** Lower-case letters, digits and hyphens, 32 at most: what a profile's id may be (§8.1). */
export const slug = text => String(text).toLowerCase().replace(/[^a-z0-9-]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 32);
export const isProfileId = id => /^[a-z0-9-]{1,32}$/.test(id);
/** `<group>-<name>` (§7.1): a group of two or three letters, then at least one more part. */
export const isLayoutId = id => /^[a-z]{2,3}(-[a-z0-9]+)+$/.test(id) && id.length <= 48;

// ---------- keys ----------

/** `on` read: `{code, mods}`, `{pair: [a, b]}`, or null when it isn't a chord. */
export function parseOn(on) {
  if (typeof on !== "string" || !on) return null;
  const parts = on.split("+");
  const code = parts.pop();
  if (!/^[A-Z][A-Za-z0-9]*$/.test(code)) return null;
  if (parts.length === 1 && LANGUAGE_KEYS.includes(parts[0]) && LANGUAGE_KEYS.includes(code) && parts[0] !== "CapsLock" && code !== "CapsLock") {
    return parts[0] === code ? null : { pair: [parts[0], code] };
  }
  let last = -1;
  for (const m of parts) {
    const at = MODIFIERS.indexOf(m);
    if (at <= last) return null;
    last = at;
  }
  return { code, mods: parts };
}

/** `on` as it is written, from a key press: the modifiers in the order ctrl, alt, shift, meta. */
export const chordOf = ({ code, ctrl, alt, shift, meta }) => [ctrl && "ctrl", alt && "alt", shift && "shift", meta && "meta", code].filter(Boolean).join("+");

const keyName = code => KEY_NAMES[code] ?? code.replace(/^(Key|Digit)/, "");
/** What a chord is called on a platform: `["⇧かな"]`, or two names for a pair. */
export function keyLabels(on, platform = "mac") {
  const p = parseOn(on);
  if (!p) return [String(on)];
  if (p.pair) return p.pair.map(keyName);
  const names = MODIFIER_NAMES[platform] ?? MODIFIER_NAMES.web;
  return [[...p.mods.map(m => names[m]), keyName(p.code)].join(names.join)];
}

/**
 * Whether the harness sees the key a binding is on, from the published state's `harness.keys`
 * (§9.3): a language key must be among `available`, and any other key needs `chords`.
 * `keys` null: nothing is known (the input method isn't running), and everything counts as seen.
 */
export function seen(on, keys) {
  const p = parseOn(on);
  if (!p || !keys) return true;
  const has = code => (keys.available ?? []).includes(code);
  if (p.pair) return p.pair.every(has);
  return LANGUAGE_KEYS.includes(p.code) ? has(p.code) : keys.chords !== false;
}

const FIXED_ACTIONS = {
  "mode.toggle": "Toggle muhenkan and henkan", "mode:muhenkan": "Go to muhenkan", "mode:henkan": "Go to henkan",
  "profile.first": "Go to the first profile", "profile.next": "Next profile of the cycle", "profile.prev": "Previous profile of the cycle",
  "profile.last": "Back to the profile before", "menu.next": "Next entry", "menu.prev": "Previous entry", fix: "Fix what shows",
  "width.full": "Make it full-width", "width.half": "Make it half-width", pass: "Give the key to the app", none: "Do nothing",
};
/** The actions that always apply (§9.2): after one of them nothing else in the list can run. */
const ALWAYS = ["mode.toggle", "width.full", "width.half", "pass", "none"];

/**
 * What an action is called. `layouts`: `[{id, name, actions: [{id, label}]}]`.
 * Returns `{text, missing}`: `missing` when it names a profile, a layout or an action that isn't there.
 */
export function actionLabel(action, file, layouts = []) {
  if (FIXED_ACTIONS[action]) return { text: FIXED_ACTIONS[action], missing: false };
  if (typeof action !== "string") return { text: String(action), missing: true };
  if (action.startsWith("profile:")) {
    const p = profileOf(file, action.slice(8));
    return p ? { text: `Go to ${p.name || p.id}`, missing: false } : { text: `Go to “${action.slice(8)}”`, missing: true };
  }
  const dot = action.indexOf(".");
  const layout = dot > 0 ? layouts.find(l => l.id === action.slice(0, dot)) : null;
  const found = layout?.actions.find(a => a.id === action.slice(dot + 1));
  return found ? { text: `${layout.name}: ${found.label}`, missing: false } : { text: action, missing: true };
}

/** Every action a binding can list, as `[id, label]`, in the order a menu offers them. */
export function allActions(file, layouts = []) {
  const fixed = id => [id, FIXED_ACTIONS[id]];
  return [
    ...["mode.toggle", "mode:muhenkan", "mode:henkan"].map(fixed),
    ...profiles(file).map(p => [`profile:${p.id}`, `Go to ${p.name || p.id}`]),
    ...["profile.first", "profile.next", "profile.prev", "profile.last", "menu.next", "menu.prev", "fix", "width.full", "width.half"].map(fixed),
    ...layouts.flatMap(l => l.actions.map(a => [`${l.id}.${a.id}`, `${l.name}: ${a.label}`])),
    ...["pass", "none"].map(fixed),
  ];
}

/** A binding in words, by the rules of §9.2: the first action that applies runs. */
export function says(binding, file, layouts = []) {
  const list = Array.isArray(binding.do) ? binding.do : [];
  if (!list.length) return "No action listed: the key is swallowed.";
  const part = action => {
    const text = lower(actionLabel(action, file, layouts).text);
    if (action === "menu.next" || action === "menu.prev") return `${text} while the menu has two rows or more`;
    if (action === "fix") return "fix what shows when something is staged";
    if (action.startsWith("profile") || action === "mode:muhenkan" || action === "mode:henkan") return `${text} unless already there`;
    if (!ALWAYS.includes(action)) return `${text} when its layout is in the profile in use`;
    return text;
  };
  const end = list.findIndex(a => ALWAYS.includes(a));
  const run = end < 0 ? list : list.slice(0, end + 1);
  return capital(run.map(part).join("; otherwise ")) + (end < 0 ? "; otherwise nothing." : ".");
}
const lower = s => s.charAt(0).toLowerCase() + s.slice(1);
const capital = s => s.charAt(0).toUpperCase() + s.slice(1);

// ---------- changes that touch more than one place ----------

/** Renames a profile's id everywhere the file names it: bindings, their `when`, the cycle, `henkan_enters`. */
export function renameProfile(file, from, to) {
  const p = profileOf(file, from);
  if (!p || from === to) return;
  p.id = to;
  const keys = file.keys;
  for (const b of Array.isArray(keys.bindings) ? keys.bindings : []) {
    if (Array.isArray(b.do)) b.do = b.do.map(a => (a === `profile:${from}` ? `profile:${to}` : a));
    if (b.when === `profile:${from}`) b.when = `profile:${to}`;
  }
  if (Array.isArray(keys.cycle)) keys.cycle = keys.cycle.map(id => (id === from ? to : id));
  if (keys.henkan_enters === from) keys.henkan_enters = to;
}

/** Takes a profile out of the list, the cycle and `henkan_enters`. Bindings that name it stay, and say so. */
export function removeProfile(file, id) {
  const list = profiles(file);
  const at = list.findIndex(p => p.id === id);
  if (at < 0) return;
  list.splice(at, 1);
  if (Array.isArray(file.keys.cycle)) file.keys.cycle = file.keys.cycle.filter(x => x !== id);
  if (file.keys.henkan_enters === id) delete file.keys.henkan_enters;
}

/** The first run's easy mode (§8.3): the muhenkan layout, and one profile `main` with one layout. */
export function easy(file, muhenkan, henkan, name) {
  file.engine.muhenkan.layout = muhenkan;
  const id = freeId(profiles(file).map(p => p.id), "main");
  profiles(file).push({ id, name: name || henkan, layouts: [henkan] });
  return id;
}

// ---------- layouts ----------

/**
 * A layout's file as lists show it: `{id, name, lang, kind, read, actions, forms, options, stock,
 * custom, modified, text}`. `language`: the page's (`en`, `ko`, `ja`), for the name and the labels.
 */
export function layoutInfo(text, language = "en", where = {}) {
  let file;
  try { file = JSON.parse(text); } catch { return null; }
  if (!file || typeof file.id !== "string") return null;
  const pick = texts => (texts && typeof texts === "object" ? texts[language] ?? texts.en ?? Object.values(texts)[0] : null);
  return {
    id: file.id, name: pick(file.name) ?? file.id, lang: file.lang ?? "und", kind: file.kind ?? "henkan", read: file.read ?? "",
    actions: (Array.isArray(file.actions) ? file.actions : []).filter(a => a && typeof a.id === "string").map(a => ({ id: a.id, label: pick(a.label) ?? a.id })),
    forms: (Array.isArray(file.forms) ? file.forms : []).map(formName),
    options: Object.entries(obj(file.options)).map(([name, o]) => ({ name, type: o?.type, of: o?.of ?? [], default: o?.default, title: pick(o?.title) ?? name, hint: pick(o?.hint) })),
    stock: false, custom: false, modified: false, text, ...where,
  };
}

/** What a form writes (§7.5.12), in words. */
export function formName(form) {
  const scripts = { hiragana: "hiragana", katakana: "katakana", "halfwidth-katakana": "half-width katakana" };
  if (form?.do === "script") return `the reading in ${scripts[form.to] ?? form.to}`;
  if (form?.do === "keys") return `the keys as typed, ${form.width === "half" ? "half-width" : "full-width"}`;
  if (form?.do === "jamo") return `the ${String(form.keep).replace(/s$/, "")} of each syllable`;
  return JSON.stringify(form);
}

/**
 * The layouts the page lists: the stock ones, the user's own after them, and a file of the user's
 * with a stock id in that one's place, marked changed (§7.6).
 */
export function catalog(stock, own, language = "en") {
  const mine = new Map(own.map(l => [l.id, l]));
  const out = [];
  for (const s of stock) {
    const changed = mine.get(s.id);
    const info = layoutInfo((changed ?? s).text, language, { stock: true, modified: !!changed, stockText: s.text });
    if (info) out.push(info);
    mine.delete(s.id);
  }
  for (const l of mine.values()) {
    const info = layoutInfo(l.text, language, { custom: true });
    if (info) out.push(info);
  }
  return out;
}

/** What a search of the layouts looks through (lower case): names in every language, the id, how it reads, where it is used. */
export function layoutWords(layout, usedIn = []) {
  const langs = { en: "english 영어 英語", ko: "korean 한국어 韓国語", ja: "japanese 일본어 日本語", und: "unicode 유니코드 ユニコード" };
  let names = [];
  try { names = Object.values(JSON.parse(layout.text).name ?? {}); } catch { /* the name as listed is enough */ }
  return [layout.name, ...names, layout.id, langs[layout.lang] ?? layout.lang, READS[layout.read] ?? layout.read, layout.kind, ...usedIn,
    layout.custom ? "yours custom" : layout.modified ? "stock changed" : "stock", layout.forms.length ? "forms" : ""].join(" ").toLowerCase();
}
export const READS = { direct: "types directly", hangul: "composes Hangul", table: "composes from spellings", kana: "composes kana", search: "searches" };

// ---------- the dictionary ----------

/** The dictionary's text read: `{positive, negative, rest}`, or null when it isn't one. No text: an empty one. */
export function readDictionary(text) {
  if (text == null || !String(text).trim()) return { positive: [], negative: [], rest: {} };
  let file;
  try { file = JSON.parse(text); } catch { return null; }
  if (!file || file.format !== DICTIONARY_FORMAT) return null;
  const { format, version, positive, negative, ...rest } = file;
  const words = list => (Array.isArray(list) ? list.filter(w => w && typeof w === "object") : []);
  return { positive: words(positive), negative: words(negative), rest };
}
export const printDictionary = d => JSON.stringify({ format: DICTIONARY_FORMAT, version: 1, ...d.rest, positive: d.positive, negative: d.negative }, null, 2) + "\n";
