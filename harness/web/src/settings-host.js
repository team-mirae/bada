// The web harness as a host of the settings page (settings/README.md, "The host's interface"):
// the folder is this browser's local storage, and the engine is the page's own. No DOM here.

import * as S from "./settings.js";

const print = value => JSON.stringify(value, null, 2) + "\n";

/** `h`: a function that returns the harness. `stock`: one that returns the stock layouts' files by id. */
export function webHost(h, stock) {
  const files = () => {
    const settings = h().store.text(S.STORE.settings), dictionary = h().store.text(S.STORE.dictionary);
    return {
      settings: settings == null ? null : { text: settings, modified: 0 },
      dictionary: dictionary == null ? null : { text: dictionary, modified: 0 },
      layouts: Object.entries(h().customLayouts).map(([id, value]) => ({ id, text: print(value), modified: 0 })),
      history: [],   // nothing is kept of earlier files here
    };
  };
  const checked = (result, extra = {}) => ({ ok: !!result.ok, problems: result.problems ?? [], layouts: h().layouts, ...extra });
  const check = what => {
    if (typeof what.settings === "string") return checked(h().loadSettings(what.settings, false, true));
    if (typeof what.layout === "string") return checked(h().loadLayout(what.layout, true));
    if (typeof what.dictionary === "string") return checked(dictionaryProblems(what.dictionary));
    return { ok: false, problems: [{ path: "", code: "bad-value", message: "nothing to check", level: "error" }], layouts: h().layouts };
  };
  // The engine takes a dictionary or refuses it: there is no dry run, so only its shape is looked at.
  const dictionaryProblems = text => {
    try { JSON.parse(text); return { ok: true, problems: [] }; }
    catch (e) { return { ok: false, problems: [{ path: "", code: "bad-json", message: `the dictionary isn't JSON: ${e.message}`, level: "error" }] }; }
  };
  const layoutId = name => /^layouts\/([a-z0-9-]+)\.json$/.exec(name)?.[1] ?? null;
  const refused = message => ({ ok: false, problems: [{ path: "", code: "bad-value", message, level: "error" }], files: files() });

  return {
    info: async () => ({ platform: "web", harness: S.WEB, version: h().engine.describe.engine?.version ?? "", folder: "this browser's local storage", embedded: true }),
    describe: async () => h().engine.describe,
    stock: async () => Object.values(stock()).map(layout => ({ id: layout.id, text: print(layout) })),
    files: async () => files(),
    check: async what => check(what),
    write: async (name, text) => {
      let result;
      if (name === "settings.json") result = h().loadSettings(text);
      else if (name === "dictionary.json") result = h().loadDictionary(text);
      else if (layoutId(name)) {
        let id = null;
        try { id = JSON.parse(text).id; } catch { /* loadLayout says so */ }
        if (id != null && id !== layoutId(name)) return refused(`${name} holds the layout ${id}`);
        result = h().loadLayout(text);
      } else return refused(`${name} is no file of the settings folder`);
      return checked(result, { files: files() });
    },
    remove: async name => {
      if (name === "dictionary.json") return { ...checked(h().loadDictionary("")), files: files() };
      if (layoutId(name)) return { ...checked(h().removeLayout(layoutId(name))), files: files() };
      return refused(`${name} can't be removed`);
    },
    undo: async () => refused("this browser keeps no earlier files"),
    state: async () => h().published(),
    reveal: async () => ({ ok: false }),
  };
}
