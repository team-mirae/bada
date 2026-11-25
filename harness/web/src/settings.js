// The settings file (the standard, §8, §9, §10.1): one bada-settings/2 object, kept in the
// browser's local storage and moved to the other harnesses as a file. Its `engine` part goes to
// the engine, `keys` is the key table (keytable.js), and `harness["com.teammirae.bada.web"]` is
// this page's own. The user's layouts and dictionary are kept beside it.
// No DOM here: the storage is passed in (localStorage on the page, a Map in the tests).

import { check as checkKeys } from "./keytable.js";

export const FORMAT = "bada-settings/2";
export const WEB = "com.teammirae.bada.web";

/** Where things are kept in local storage. */
export const STORE = {
  settings: "bada.settings",       // the settings file, as text
  layouts: "bada.layouts",         // the user's layouts: {id: bada-layout/3 object}
  dictionary: "bada.dictionary",   // the user dictionary: a bada-dictionary object, as text
  last: "bada.web.last",           // the mode and profile when the page was last used
  seen: "bada.web.seen",           // the preset of the language keys the page has seen
};

/** This page's own settings, and what each may be. The first is the default. */
export const OWN = {
  keyboard: ["auto", "jis-mac", "jis-pc", "none"],   // the keyboard the key preset is for, when the file has no keys.bindings (§9.5)
  board: ["auto", "hhkb", "apple", "pc"],            // the keyboard drawn on the page
};

const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);

/** A new install's file (§8.3): no muhenkan layout and no henkan profile. */
export function fresh(describe) {
  const engine = describe?.settings?.defaults ?? { muhenkan: { layout: null }, henkan: { profiles: [] } };
  return { format: FORMAT, engine: structuredClone(engine), keys: {}, harness: {} };
}

/** This page's own settings in a file, with their defaults. */
export function own(file) {
  const mine = isObject(file?.harness?.[WEB]) ? file.harness[WEB] : {};
  return Object.fromEntries(Object.entries(OWN).map(([name, of]) => [name, of.includes(mine[name]) ? mine[name] : of[0]]));
}

/** The file with one of this page's own settings changed. */
export function withOwn(file, name, value) {
  const next = structuredClone(file);
  next.harness = isObject(next.harness) ? next.harness : {};
  next.harness[WEB] = { ...(isObject(next.harness[WEB]) ? next.harness[WEB] : {}), [name]: value };
  return next;
}

/** Easy mode of the onboarding (§8.3): the muhenkan layout, and one profile with one layout. */
export function withEasy(file, muhenkan, henkan, name) {
  const next = structuredClone(file);
  next.engine = isObject(next.engine) ? next.engine : {};
  next.engine.muhenkan = { ...(isObject(next.engine.muhenkan) ? next.engine.muhenkan : {}), layout: muhenkan };
  next.engine.henkan = isObject(next.engine.henkan) ? next.engine.henkan : {};
  const profiles = Array.isArray(next.engine.henkan.profiles) ? next.engine.henkan.profiles : [];
  let id = "main";
  for (let n = 2; profiles.some(p => p?.id === id); n++) id = `main-${n}`;
  next.engine.henkan.profiles = [...profiles, { id, name, layouts: [henkan] }];
  return next;
}

/**
 * What is wrong with a settings file, apart from its `engine` part, which the engine checks
 * itself: `[{path, code, message, level}]`, as §6.6 has them. `profiles` and `layouts` are what
 * the key table's actions may name.
 */
export function check(file, profiles = [], layouts = []) {
  const problems = [];
  const error = (path, code, message) => problems.push({ path, code, message, level: "error" });
  const warning = (path, code, message) => problems.push({ path, code, message, level: "warning" });
  if (!isObject(file)) {
    error("", "bad-value", "a settings file is a JSON object");
    return problems;
  }
  if (file.format !== FORMAT) error("format", "bad-format", `format is "${FORMAT}"`);
  for (const name of Object.keys(file)) {
    if (!["format", "engine", "keys", "harness"].includes(name)) error(name, "unknown-field", `a settings file has no part ${name}`);
  }
  if (file.engine !== undefined && !isObject(file.engine)) error("engine", "bad-value", "engine is an object");
  problems.push(...checkKeys(file.keys, profiles, layouts));
  if (file.harness !== undefined && !isObject(file.harness)) error("harness", "bad-value", "harness is an object of harness ids");
  for (const [id, part] of Object.entries(isObject(file.harness) ? file.harness : {})) {
    if (!/^[a-z0-9]+(\.[A-Za-z0-9-]+)+$/.test(id)) error(`harness.${id}`, "bad-value", "a harness's settings are under its id");
    if (!isObject(part)) error(`harness.${id}`, "bad-value", "a harness's settings are an object");
  }
  const mine = file.harness?.[WEB];
  for (const [name, value] of Object.entries(isObject(mine) ? mine : {})) {
    const path = `harness.${WEB}.${name}`;
    if (!OWN[name]) warning(path, "unknown-field", `this page has no setting ${name}: it is kept and not used`);
    else if (!OWN[name].includes(value)) error(path, "bad-value", `${name} is one of ${OWN[name].join(", ")}`);
  }
  return problems;
}

/** Local storage, read and written as JSON. A storage that refuses (private mode, full) says so. */
export class Store {
  constructor(storage) {
    this.storage = storage;
  }

  text(key) {
    try { return this.storage.getItem(key); } catch { return null; }
  }

  /** `{value}`, or `{problem}` when what is kept isn't JSON. Nothing kept: `{value: undefined}`. */
  json(key) {
    const text = this.text(key);
    if (text == null) return { value: undefined };
    try { return { value: JSON.parse(text) }; } catch (e) { return { problem: `what is kept under ${key} isn't JSON: ${e.message}`, text }; }
  }

  /** Writes text, or removes the key when it is null. Returns an error message, or null. */
  write(key, text) {
    try {
      if (text == null) this.storage.removeItem(key);
      else this.storage.setItem(key, text);
      return null;
    } catch (e) {
      return `the browser didn't keep ${key}: ${e.message}`;
    }
  }
}

/** The settings file as text, the way it is written to a file and shown in the editor. */
export const print = file => JSON.stringify(file, null, 2) + "\n";
