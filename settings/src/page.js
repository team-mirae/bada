// The settings page (the standard, §10.1): `mount(root, host)` draws it into `root` and keeps the
// settings folder as the user changes it, through `host` (settings/README.md has its interface).
// Changes apply at once: each one is written, the host checks it, and what is wrong shows at the top.

import * as M from "./model.js";

const TAB_OF = { mac: "This Mac", windows: "This PC", linux: "This computer", web: "This page" };
const LANGUAGE = ["ko", "ja"].find(l => (globalThis.navigator?.language ?? "en").toLowerCase().startsWith(l)) ?? "en";

function h(tag, props = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (k === "class") e.className = v;
    else if (v === true) e.setAttribute(k, "");
    else if (v !== false && v != null) { if (k in e && k !== "list") e[k] = v; else e.setAttribute(k, v); }
  }
  for (const c of children.flat(Infinity)) if (c != null && c !== false) e.append(c);
  return e;
}
const hint = (...t) => h("p", { class: "hint" }, ...t);
const row = (...c) => h("div", { class: "row" }, ...c);
const grow = () => h("span", { class: "grow" });
const pill = (text, cls = "") => h("span", { class: `pill ${cls}` }, text);
const mono = text => h("span", { class: "mono" }, text);
const card = (title, small, ...c) => h("section", { class: "card" }, h("h3", {}, title, small ? h("small", {}, small) : null), ...c);
const field = (label, control) => h("div", { class: "field" }, h("span", {}, label), control);
const banner = (...c) => h("div", { class: "banner" }, ...c);
function check(label, checked, onchange, disabled = false) {
  return h("label", { class: "check" }, h("input", { type: "checkbox", checked: !!checked, disabled, onchange: e => onchange(e.target.checked) }), h("span", {}, label));
}
function select(options, value, onchange, props = {}) {
  const s = h("select", { ...props, onchange: e => onchange(e.target.value) }, options.map(([v, label]) => h("option", { value: v }, label)));
  s.value = value ?? "";
  return s;
}
const when = seconds => (seconds ? new Date(seconds * 1000).toLocaleString() : "");
/** `settings-20261124-085514.json` as a date and time here: the name has them in UTC. */
function historyName(name) {
  const m = /^settings-(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})/.exec(name);
  return m ? new Date(Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6])).toLocaleString() : name;
}

/**
 * Draws the page into `root`. `options.header`: false leaves the title bar out (the web harness's
 * dialog has its own). `options.firstRun`: false leaves the first run to the host.
 * `options.layoutEditor`: a function from a layout of the list to the address of a host's own
 * editor for it, which the Layouts tab then links to. Returns
 * `{reload}`: reads the folder again.
 */
export async function mount(root, host, options = {}) {
  root.classList.add("bada-settings");
  const c = {
    info: { platform: "web", harness: "", version: "", folder: "", embedded: false }, describe: {}, stock: [], files: { settings: null, dictionary: null, layouts: [], history: [] },
    state: null, file: M.fresh(), problems: [], saved: true, saving: false, dirty: false, lost: null, unreadable: false, layouts: [], dict: { positive: [], negative: [], rest: {} }, dictBad: false,
  };
  const ui = { tab: "Profiles", profile: "@muhenkan", layout: null, query: "", json: false, editKey: null, recording: false, draft: null, checked: null, confirm: null, note: "", copyId: null };

  const top = h("div", { class: "top" });
  const status = h("div", { class: "status", "aria-live": "polite" });
  const tabs = h("nav", { class: "tabs", role: "tablist", "aria-label": "Settings" });
  const page = h("main", { class: "page" });
  const jsonText = h("pre", {});
  const json = h("aside", { class: "json", hidden: true }, h("header", {}, h("span", {}, "settings.json"), h("span", {}, M.FORMAT)), jsonText);
  const body = h("div", { class: "body" }, page, json);
  const first = firstRun();
  root.replaceChildren(...(options.header === false ? [] : [top]), status, tabs, body, first.dialog);

  const profiles = () => M.profiles(c.file);
  const layoutOf = id => c.layouts.find(l => l.id === id);
  const layoutName = id => layoutOf(id)?.name ?? id;
  const own = () => c.file.harness[c.info.harness] ?? {};
  const keysSeen = () => c.state?.harness?.keys ?? null;
  const presetName = () => (own().keyboard && own().keyboard !== "auto" ? own().keyboard : keysSeen()?.preset ?? M.platformPreset(c.info.platform));

  // ---------- reading and writing ----------

  async function call(f) {
    try { return await f(); } catch (e) { c.lost = String(e.message ?? e); drawStatus(); return null; }
  }
  function take(files) {
    if (!files) return;
    c.files = files;
    c.layouts = M.catalog(c.stock, files.layouts ?? [], LANGUAGE);
    const d = M.readDictionary(files.dictionary?.text);
    c.dictBad = d == null;
    c.dict = d ?? { positive: [], negative: [], rest: {} };
  }
  async function reload() {
    const [info, describe, stock, files, state] = await Promise.all([host.info(), host.describe(), host.stock(), host.files(), host.state()].map(p => call(() => p)));
    if (info) c.info = info;
    if (describe) c.describe = describe;
    if (stock) c.stock = stock;
    c.state = state ?? null;
    take(files);
    const text = c.files.settings?.text;
    const file = text == null ? M.fresh() : M.read(text);
    c.unreadable = text != null && !file;
    c.file = file ?? M.fresh();
    if (text != null) {
      const checked = await call(() => host.check({ settings: text }));
      c.problems = checked?.problems ?? [];
      c.saved = checked?.ok ?? true;
    }
    if (!ui.layout) ui.layout = c.layouts[0]?.id ?? null;
    if (!M.profileOf(c.file, ui.profile)) ui.profile = profiles()[0]?.id ?? "@muhenkan";
    draw();
    // A new install: the first run's two choices (§8.3).
    if (options.firstRun !== false && (text == null || (!profiles().length && !c.file.engine.muhenkan.layout))) first.open();
  }
  /** Writes the settings file. One write at a time: a change made meanwhile is written after it. */
  async function save() {
    if (c.saving) { c.dirty = true; return; }
    c.saving = true;
    do {
      c.dirty = false;
      const result = await call(() => host.write("settings.json", M.print(c.file)));
      if (result) {
        c.problems = result.problems ?? [];
        c.saved = !!result.ok;
        c.unreadable = false;
        take(result.files);
      }
    } while (c.dirty);
    c.saving = false;
    drawStatus();
    if (ui.tab === "Files") drawPage();
  }
  /** Changes the file, shows it, and writes it. */
  function change(f) { f(c.file); draw(); save(); }
  /** Changes only what the page shows. */
  function show(f) { f(); draw(); }
  async function refreshState() {
    c.state = (await call(() => host.state())) ?? null;
    drawPage();
  }

  // ---------- the first run ----------

  function firstRun() {
    const muhenkan = h("select", { id: "bada-first-muhenkan" }), henkan = h("select", { id: "bada-first-henkan" });
    const dialog = h("dialog", { class: "first", "aria-labelledby": "bada-first-title" },
      h("div", { class: "dialog-head" }, h("b", { id: "bada-first-title" }, "Start with bada"), h("button", { class: "quiet", "aria-label": "Close", onclick: () => dialog.close() }, "×")),
      h("div", { class: "dialog-body" },
        h("p", {}, "bada has two modes. ", h("b", {}, "Muhenkan"), " types directly. ", h("b", {}, "Henkan"), " composes text, and has as many profiles as you make. A new install has no layout for either: pick one for each."),
        h("label", { class: "field" }, h("span", {}, "Muhenkan types"), muhenkan),
        h("label", { class: "field" }, h("span", {}, "Henkan composes"), henkan),
        row(h("button", { class: "primary", onclick: () => { const id = henkan.value; dialog.close(); change(f => { ui.profile = M.easy(f, muhenkan.value, id, layoutName(id)); ui.tab = "Profiles"; }); } }, "Start"),
          h("button", { onclick: () => dialog.close() }, "Skip: the full settings")),
        hint("Start writes the muhenkan layout and one profile, ", mono("main"), ", with that one layout. The key that enters henkan works from then on.")));
    return { dialog, open() {
      const direct = c.layouts.filter(l => l.kind === "muhenkan"), composing = c.layouts.filter(l => l.kind !== "muhenkan");
      if (!direct.length || !composing.length || dialog.open) return;
      muhenkan.replaceChildren(...direct.map(l => h("option", { value: l.id }, l.name)));
      henkan.replaceChildren(...composing.map(l => h("option", { value: l.id }, l.name)));
      muhenkan.value = direct.find(l => l.id === "en-qwerty")?.id ?? direct[0].id;
      henkan.value = composing.find(l => l.id === (LANGUAGE === "ko" ? "ko-2-set" : "ja-romaji"))?.id ?? composing[0].id;
      dialog.showModal();
    } };
  }

  // ---------- Profiles ----------

  function profilesPage() {
    const sel = ui.profile;
    const list = h("div", { class: "list", role: "list" },
      h("button", { class: "item", "aria-current": String(sel === "@muhenkan"), onclick: () => show(() => { ui.profile = "@muhenkan"; }) },
        h("b", {}, h("span", { class: "badge plain" }, "A"), "Muhenkan"), h("small", {}, c.file.engine.muhenkan.layout ? layoutName(c.file.engine.muhenkan.layout) : "the system's layout")),
      profiles().map(p => h("button", { class: "item", "aria-current": String(sel === p.id), onclick: () => show(() => { ui.profile = p.id; ui.note = ""; }) },
        h("b", {}, h("span", { class: "badge" }, [...(p.name || p.id || "?")][0]), p.name || p.id), h("small", {}, (p.layouts ?? []).map(layoutName).join(" · ") || "no layout"))),
      h("button", { onclick: () => change(f => { const id = M.freeId(profiles().map(p => p.id), "new"); M.profiles(f).push({ id, name: "New profile", layouts: [] }); ui.profile = id; ui.note = ""; }) }, "+ New profile"));
    return [h("div", { class: "split" }, list, sel === "@muhenkan" || !M.profileOf(c.file, sel) ? muhenkanDetail() : profileDetail(M.profileOf(c.file, sel)))];
  }

  function muhenkanDetail() {
    const k = c.file.keys;
    return h("div", { class: "stack" },
      card("Muhenkan", "types directly",
        field("Layout", select([["", "None: the system's own layout types"], ...c.layouts.filter(l => l.kind === "muhenkan").map(l => [l.id, `${l.name}  (${l.id})`])], c.file.engine.muhenkan.layout ?? "",
          v => change(f => { if (v) f.engine.muhenkan.layout = v; else delete f.engine.muhenkan.layout; }), { "aria-label": "Muhenkan layout" })),
        hint("One layout that types what its keys say. With none, as on a new install, bada passes every key on.")),
      card("Caps Lock", "in the Keys tab",
        hint("Unbound, the key is the lock: ", h("b", {}, k.caps_lock === "transient" ? "in muhenkan only" : "everywhere"), ", typing ", h("b", {}, k.caps_rule === "upper_snake" ? "UPPER_SNAKE" : "ALL CAPS"), ".")),
      profiles().length ? null : card("No henkan profile yet", "", hint("A profile holds one layout or more, and what you type is read by all of them at once."),
        row(h("button", { class: "primary", onclick: () => first.open() }, "Easy start…"))));
  }

  function profileDetail(p) {
    const list = profiles(), i = list.indexOf(p);
    p.layouts = Array.isArray(p.layouts) ? p.layouts : [];
    const move = (from, by) => change(() => { const to = from + by; if (to < 0 || to >= p.layouts.length) return; [p.layouts[from], p.layouts[to]] = [p.layouts[to], p.layouts[from]]; });
    const candidates = p.layouts.map((id, at) => h("div", { class: "cand" },
      h("span", { class: "order" }, h("button", { "aria-label": "Earlier", disabled: at === 0, onclick: () => move(at, -1) }, "▲"), h("button", { "aria-label": "Later", disabled: at === p.layouts.length - 1, onclick: () => move(at, 1) }, "▼")),
      h("span", { class: "name" }, layoutName(id), " ", h("span", { class: "mono muted" }, id)),
      layoutOf(id) ? pill(M.READS[layoutOf(id).read] ?? layoutOf(id).read) : pill("not installed", "warn"),
      h("label", { class: "row", title: "Multiplies the score of everything this layout offers" }, "×", h("input", { type: "number", min: 0, max: 10, step: 0.1, value: p.weights?.[id] ?? 1, "aria-label": `Weight of ${layoutName(id)}`,
        onchange: e => change(() => { const w = Math.min(10, Math.max(0, Number(String(e.target.value).replace(",", ".")))); if (!Number.isFinite(w)) return; p.weights ??= {}; if (w === 1) delete p.weights[id]; else p.weights[id] = w; if (!Object.keys(p.weights).length) delete p.weights; }) })),
      h("button", { class: "quiet", "aria-label": `Remove ${layoutName(id)}`, onclick: () => change(() => { p.layouts.splice(at, 1); }) }, "×")));
    const rest = c.layouts.filter(l => !p.layouts.includes(l.id));
    const from = p.layouts.map(layoutOf).filter(l => l && l.forms.length);
    const formsLayout = from.find(l => l.id === p.script_forms_layout) ?? from[0];
    const actions = [["swallow", "Swallowed (does nothing)"], ["pass", "Passed to the app"], ...p.layouts.flatMap(id => (layoutOf(id)?.actions ?? []).map(a => [`${id}.${a.id}`, `${layoutName(id)}: ${a.label}`]))];
    const fk = p.fkeys ?? { intercept: true, keys: {} };
    const setF = f => change(() => { p.fkeys ??= { intercept: true, keys: {} }; p.fkeys.keys ??= {}; f(p.fkeys); });
    const japanese = p.layouts.find(id => layoutOf(id)?.actions.some(a => a.id === "katakana"));
    const withOptions = p.layouts.map(layoutOf).filter(l => l && l.options.length);
    const bound = (Array.isArray(c.file.keys.bindings) ? c.file.keys.bindings : []).filter(b => (b.do ?? []).includes(`profile:${p.id}`));
    return h("div", { class: "stack" },
      card("Profile", `${i + 1} of ${list.length}`,
        field("Name", h("input", { type: "text", value: p.name ?? "", "aria-label": "Name", onchange: e => change(() => { p.name = e.target.value || p.id; }) })),
        field("Id", h("input", { type: "text", class: "mono", value: p.id, "aria-label": "Id", spellcheck: false, onchange: e => {
          const to = e.target.value.trim();
          if (to === p.id) return;
          if (!M.isProfileId(to)) return show(() => { ui.note = "An id is 1 to 32 of a–z, 0–9 and -."; });
          if (M.profileOf(c.file, to)) return show(() => { ui.note = `Another profile is already ${to}.`; });
          change(f => { M.renameProfile(f, p.id, to); ui.profile = to; ui.note = ""; });
        } })),
        ui.note ? h("p", { class: "errline" }, ui.note) : null,
        hint("The name is what menus show. The id is what the key table and the state use: changing it changes the keys that name it too."),
        row(h("button", { disabled: i === 0, onclick: () => change(() => { list.splice(i - 1, 0, ...list.splice(i, 1)); }) }, "Move up"),
          h("button", { disabled: i === list.length - 1, onclick: () => change(() => { list.splice(i + 1, 0, ...list.splice(i, 1)); }) }, "Move down"), grow(),
          ui.confirm === "profile" ? [h("span", { class: "muted" }, "Delete it?"), h("button", { class: "danger", onclick: () => change(f => { M.removeProfile(f, p.id); ui.profile = M.profiles(f)[0]?.id ?? "@muhenkan"; ui.confirm = null; }) }, "Delete"),
            h("button", { onclick: () => show(() => { ui.confirm = null; }) }, "Keep")] : h("button", { class: "danger", onclick: () => show(() => { ui.confirm = "profile"; }) }, "Delete profile…"))),
      card("Layouts", "every one reads the keys at once; the readings are ranked as you type",
        candidates.length ? candidates : hint("No layout yet: nothing is typed in this profile."),
        row(select([["", "Add a layout…"], ...rest.map(l => [l.id, `${l.name}  (${l.id})`])], "", v => { if (v) change(() => { p.layouts.push(v); }); }, { "aria-label": "Add a layout" }),
          hint("Earlier wins a tie. × is the weight: 1 as it is, 0 to 10."))),
      card("Forms below the top entry", "entries −1, −2, …",
        check("Show a layout's forms as negative entries: the previous entry from the top goes down into them", p.script_forms,
          v => change(() => { if (v) p.script_forms = true; else { delete p.script_forms; delete p.script_forms_layout; } }), !from.length && !p.script_forms),
        from.length > 1 ? field("Forms of", select([["", `The first that lists any (${from[0].name})`], ...from.map(l => [l.id, l.name])], p.script_forms_layout ?? "",
          v => change(() => { if (v) p.script_forms_layout = v; else delete p.script_forms_layout; }), { disabled: !p.script_forms, "aria-label": "The layout whose forms they are" })) : null,
        formsLayout ? h("div", { class: `forms${p.script_forms ? "" : " off"}` }, formsLayout.forms.map((f, n) => h("div", {}, h("i", {}, `−${n + 1}`), h("span", {}, f))),
          h("div", {}, h("i", {}, "0"), h("span", { class: "muted" }, "the top reading, then the others")))
          : hint("No layout of this profile lists forms. A layout lists its own, as many as it wants, in its file (Layouts tab)."),
        formsLayout ? hint("They are made from the keys and ", mono(`${formsLayout.id}.json`), " alone, so the same keys always give the same entries.") : null),
      card("F6–F10", "this profile's own",
        check("Take F6–F10 while this profile is in use (off: every F key goes to the app)", fk.intercept !== false, v => setF(f => { f.intercept = v; })),
        [6, 7, 8, 9, 10].map(n => { const now = fk.keys?.[`F${n}`] ?? "swallow";
          return field(`F${n}`, select(actions.some(a => a[0] === now) ? actions : [...actions, [now, `${now} (not in this profile)`]], now,
            v => setF(f => { if (v === "swallow") delete f.keys[`F${n}`]; else f.keys[`F${n}`] = v; }), { disabled: fk.intercept === false, "aria-label": `Action of F${n}` })); }),
        japanese ? row(h("button", { onclick: () => setF(f => { f.intercept = true;
          f.keys = { F6: `${japanese}.hiragana`, F7: `${japanese}.katakana`, F8: `${japanese}.halfwidth-katakana`, F9: `${japanese}.fullwidth-latin`, F10: `${japanese}.halfwidth-latin` }; }) }, "Japanese IME preset")) : null),
      withOptions.length ? card("Options of its layouts", "declared by each layout's file",
        withOptions.map(l => l.options.map(o => {
          const value = p.options?.[l.id]?.[o.name] ?? o.default ?? (o.type === "order" ? o.of : false);
          const set = v => change(() => { ((p.options ??= {})[l.id] ??= {})[o.name] = v; });
          if (o.type === "boolean") return h("div", { class: "stack tight" }, check(`${l.name}: ${o.title}`, value, set), o.hint ? hint(o.hint) : null);
          const order = Array.isArray(value) ? value : [];
          return h("div", { class: "stack tight" }, h("span", {}, `${l.name}: ${o.title}`),
            row(o.of.map(t => h("button", { class: `pill ${order.includes(t) ? "on" : ""}`, "aria-pressed": String(order.includes(t)),
              onclick: () => set(order.includes(t) ? order.filter(x => x !== t) : [...order, t]) }, order.includes(t) ? `${order.indexOf(t) + 1}. ${t}` : t))),
            hint(o.hint ?? "", " Click to add or remove; the order is the order clicked."));
        }))) : null,
      card("Keys that go here", "from the Keys tab",
        bound.length ? bound.map(b => row(keyCaps(b.on), h("span", { class: "muted" }, M.says(b, c.file, c.layouts)))) : hint("No key goes to this profile by name. The first profile and the cycle can still reach it."),
        row(h("button", { onclick: () => show(() => { ui.tab = "Keys"; }) }, "Edit in Keys"))));
  }

  // ---------- Keys ----------

  const keyCaps = on => M.keyLabels(on, c.info.platform).map(name => h("kbd", {}, name)).flatMap((k, i) => (i ? [" + ", k] : [k]));

  function keysPage() {
    const k = c.file.keys, seenKeys = keysSeen();
    const mine = Array.isArray(k.bindings);
    const list = mine ? k.bindings : M.preset(presetName());
    const plain = code => list.filter(b => { const p = M.parseOn(b.on); return p && !p.pair && p.code === code && !p.mods.length; });
    const strip = h("div", { class: "strip" }, M.LANGUAGE_KEYS.map(code => {
      const on = !seenKeys || (seenKeys.available ?? []).includes(code), bs = plain(code);
      return h("div", { class: `keytile${on ? "" : " off"}` }, h("div", {}, h("kbd", {}, M.KEY_NAMES[code])),
        h("small", {}, !on ? "Not seen here" : bs.length ? bs.map(b => (b.do ?? []).map(a => M.actionLabel(a, c.file, c.layouts).text).join(", else ")).join(" · ")
          : code === "CapsLock" ? "The lock" : "Unbound: the system's"));
    }));
    const rows = list.map((b, i) => {
      const active = M.seen(b.on, seenKeys), editing = mine && ui.editKey === i;
      return h("div", { class: `binding${active ? "" : " inactive"}` },
        h("div", { class: "on stack tight" }, h("div", {}, keyCaps(b.on)),
          mine && !editing ? h("button", { class: "quiet start", onclick: () => show(() => { ui.editKey = i; ui.recording = false; }) }, "Change key…") : null),
        h("div", { class: "does" }, (b.do ?? []).map((a, j) => { const l = M.actionLabel(a, c.file, c.layouts);
            return [j ? h("span", { class: "else" }, "else") : null, h("span", { class: `chip${l.missing ? " warn" : ""}`, title: a }, l.text,
              mine ? h("button", { "aria-label": `Remove ${l.text}`, onclick: () => change(() => { b.do.splice(j, 1); }) }, "×") : null)]; }),
          mine ? select([["", (b.do ?? []).length ? "+ else…" : "+ what it does…"], ...M.allActions(c.file, c.layouts)], "", v => { if (v) change(() => { (b.do ??= []).push(v); }); }, { class: "small", "aria-label": "Add an action" }) : null),
        h("div", { class: "row end" },
          active ? null : pill(seenKeys?.missing?.length ? `needs ${seenKeys.missing.join(", ")}` : "not seen here", "warn"),
          mine ? select([["", "Always"], ["muhenkan", "In muhenkan"], ["henkan", "In henkan"], ...profiles().map(p => [`profile:${p.id}`, `In ${p.name || p.id}`]),
            ...(b.when?.startsWith("profile:") && !M.profileOf(c.file, b.when.slice(8)) ? [[b.when, `In “${b.when.slice(8)}”`]] : [])], b.when ?? "",
            v => change(() => { if (v) b.when = v; else delete b.when; }), { class: "small", "aria-label": "When it holds" }) : pill(b.when ? `in ${b.when.replace("profile:", "")}` : "always"),
          mine ? h("button", { class: "quiet", "aria-label": "Remove this binding", onclick: () => change(() => { k.bindings.splice(i, 1); ui.editKey = null; }) }, "×") : null),
        editing ? keyEditor(b) : h("div", { class: "says" }, M.says(b, c.file, c.layouts)));
    });
    return [
      card("Language keys", seenKeys ? "what bada sees on this keyboard now" : "bada isn't running, so it can't say which it sees", strip,
        seenKeys?.missing?.length ? banner(`Without ${seenKeys.missing.join(" and ")}, some keys aren't seen. Bindings on them are kept and marked, not dropped. `,
          h("button", { class: "link", onclick: () => show(() => { ui.tab = TAB_OF[c.info.platform]; }) }, "Allow it")) : null),
      card("Bindings", mine ? "this file's own" : `the preset “${M.PRESET_NAMES[presetName()]}”: this file has none of its own`,
        rows,
        mine ? row(h("button", { onclick: () => change(() => { k.bindings.push({ on: "F13", do: [] }); ui.editKey = k.bindings.length - 1; ui.recording = false; }) }, "+ New binding"), grow(),
            ui.confirm === "preset" ? [h("span", { class: "muted" }, "Drop these bindings?"), h("button", { class: "danger", onclick: () => change(() => { delete k.bindings; ui.confirm = null; ui.editKey = null; }) }, "Use the preset"),
              h("button", { onclick: () => show(() => { ui.confirm = null; }) }, "Keep")] : h("button", { onclick: () => show(() => { ui.confirm = "preset"; }) }, "Use the keyboard's preset instead…"))
          : row(h("button", { class: "primary", onclick: () => change(() => { k.bindings = M.preset(presetName()); }) }, "Customize: copy the preset into the file")),
        hint("The first action that applies runs. One doesn't apply when it would change nothing: a menu action with one row or none, a profile that is already in use. Among bindings on one key, a profile's wins over a mode's, and that over “always”; among equals, the later one.")),
      card("Entering henkan, and Caps Lock", "",
        field("Henkan enters", select([["last", "The profile last used"], ...profiles().map(p => [p.id, p.name || p.id])], k.henkan_enters ?? "last",
          v => change(() => { if (v === "last") delete k.henkan_enters; else k.henkan_enters = v; }), { "aria-label": "The profile henkan is entered with" })),
        field("The cycle", profiles().length ? h("div", { class: "row" }, profiles().map(p => { const cycle = Array.isArray(k.cycle) ? k.cycle : [], on = cycle.includes(p.id);
          return h("button", { class: `pill ${on ? "on" : ""}`, "aria-pressed": String(on), onclick: () => change(() => { k.cycle = on ? cycle.filter(x => x !== p.id) : [...cycle, p.id]; if (!k.cycle.length) delete k.cycle; }) },
            on ? `${cycle.indexOf(p.id) + 1}. ${p.name || p.id}` : p.name || p.id); })) : h("span", { class: "muted" }, "No profile yet")),
        field("Caps Lock", select([["respect", "The lock everywhere"], ["transient", "The lock in muhenkan only"]], k.caps_lock ?? "respect",
          v => change(() => { if (v === "respect") delete k.caps_lock; else k.caps_lock = v; }), { "aria-label": "What the lock does" })),
        field("It types", select([["allcaps", "ALL CAPS"], ["upper_snake", "UPPER_SNAKE: Space types _"]], k.caps_rule ?? "allcaps",
          v => change(() => { if (v === "allcaps") delete k.caps_rule; else k.caps_rule = v; }), { "aria-label": "What the lock types in muhenkan" })),
        hint("The cycle is what “next profile” and “previous profile” go through. Bound to anything above, Caps Lock stops being the lock.")),
    ];
  }

  /** A binding's key: a language key picked from a list, or any key pressed, with its modifiers. */
  function keyEditor(b) {
    const p = M.parseOn(b.on) ?? { code: "F13", mods: [] };
    const names = { ctrl: "ctrl", alt: "alt", shift: "shift", meta: "meta" };
    const put = (code, mods) => change(() => { b.on = [...["ctrl", "alt", "shift", "meta"].filter(m => mods.includes(m)), code].join("+"); });
    if (p.pair) return h("div", { class: "says" }, row(h("span", {}, "Two language keys held together."), h("button", { onclick: () => change(() => { b.on = p.pair[0]; }) }, "Make it one key"),
      h("button", { onclick: () => show(() => { ui.editKey = null; }) }, "Done")));
    const options = [...M.LANGUAGE_KEYS.map(code => [code, M.KEY_NAMES[code]]), ...(M.LANGUAGE_KEYS.includes(p.code) ? [] : [[p.code, `${M.keyLabels(p.code)[0]} (as recorded)`]])];
    return h("div", { class: "says editor" },
      row(["ctrl", "alt", "shift", "meta"].map(m => check(M.keyLabels(`${names[m]}+KeyA`, c.info.platform)[0].replace(/\+?A$/, ""), p.mods.includes(m),
          v => put(p.code, v ? [...p.mods, m] : p.mods.filter(x => x !== m)))),
        select(options, p.code, v => put(v, p.mods), { "aria-label": "The key" }),
        h("button", { class: ui.recording ? "recording" : "", onclick: () => show(() => { ui.recording = !ui.recording; }) }, ui.recording ? "Press the key (Esc cancels)" : "Record another key…"),
        h("button", { class: "primary", onclick: () => show(() => { ui.editKey = null; ui.recording = false; }) }, "Done")),
      hint("A language key is picked from the list: while bada is the input method, pressing one does its job instead of being recorded."));
  }
  addEventListener("keydown", e => {
    if (!ui.recording || ui.tab !== "Keys" || !root.isConnected) return;
    if (["ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight"].includes(e.code) || !e.code) return;
    e.preventDefault(); e.stopPropagation();
    const b = c.file.keys.bindings?.[ui.editKey];
    ui.recording = false;
    if (b && e.code !== "Escape") change(() => { b.on = M.chordOf({ code: e.code, ctrl: e.ctrlKey, alt: e.altKey, shift: e.shiftKey, meta: e.metaKey }); });
    else draw();
  }, true);

  // ---------- Layouts ----------

  const usedIn = id => [c.file.engine.muhenkan.layout === id ? "Muhenkan" : null, ...profiles().filter(p => (p.layouts ?? []).includes(id)).map(p => p.name || p.id)].filter(Boolean);

  function layoutsPage() {
    const l = layoutOf(ui.layout) ?? c.layouts[0];
    if (!l) return [hint("The host listed no layouts.")];
    if (ui.draft?.id !== l.id) ui.draft = { id: l.id, text: l.text };
    const rows = c.layouts.map(x => h("tr", { class: "pick", "aria-current": String(x.id === l.id), "data-words": M.layoutWords(x, usedIn(x.id)), onclick: () => show(() => { ui.layout = x.id; ui.checked = null; ui.confirm = null; ui.copyId = null; }) },
      h("td", {}, x.name, h("br"), h("span", { class: "mono muted" }, x.id)),
      h("td", {}, M.READS[x.read] ?? x.read),
      h("td", {}, usedIn(x.id).join(", ") || h("span", { class: "muted" }, "—")),
      h("td", {}, x.custom ? pill("yours", "on") : x.modified ? pill("stock, changed", "warn") : pill("stock"))));
    const none = h("tr", { hidden: true }, h("td", { colspan: 4, class: "muted" }, "No layout matches. Try a language, an id, or how it reads."));
    const count = h("span", { class: "muted small", "aria-live": "polite" });
    const input = h("input", { type: "search", class: "search", placeholder: "Search layouts: a name, an id, a language, “searches”, “yours”…", value: ui.query, "aria-label": "Search layouts",
      autocomplete: "off", spellcheck: false, oninput: e => { ui.query = e.target.value; filter(); }, onkeydown: e => { if (e.key === "Escape") { ui.query = ""; e.target.value = ""; filter(); } } });
    const clear = h("button", { class: "quiet", "aria-label": "Clear the search", onclick: () => { ui.query = ""; input.value = ""; filter(); input.focus(); } }, "Clear");
    // Rows are hidden in place, so the field keeps the keyboard while you type.
    function filter() {
      const terms = ui.query.toLowerCase().split(/\s+/).filter(Boolean);
      let shown = 0;
      for (const r of rows) { r.hidden = !terms.every(w => r.dataset.words.includes(w)); if (!r.hidden) shown++; }
      none.hidden = shown > 0;
      count.textContent = terms.length ? `${shown} of ${rows.length}` : `${rows.length} layouts`;
      clear.hidden = !ui.query;
    }
    filter();
    const text = h("textarea", { class: "code", spellcheck: false, "aria-label": `${l.id}.json`, value: ui.draft.text, oninput: e => { ui.draft.text = e.target.value; } });
    const draftId = () => { try { return JSON.parse(ui.draft.text).id; } catch { return null; } };
    const checkIt = async () => { const r = await call(() => host.check({ layout: ui.draft.text })); show(() => { ui.checked = r ? { ok: r.ok, problems: r.problems ?? [], what: "Checked" } : null; }); };
    const write = async textToWrite => {
      let id; try { id = JSON.parse(textToWrite).id; } catch { id = null; }
      if (typeof id !== "string") return show(() => { ui.checked = { ok: false, problems: [{ level: "error", path: "id", code: "missing-id", message: "the file has no id" }], what: "Not saved" }; });
      const r = await call(() => host.write(`layouts/${id}.json`, textToWrite));
      if (!r) return;
      if (r.ok) { take(r.files); ui.layout = id; ui.draft = null; ui.confirm = null; ui.copyId = null; await recheck(); }
      show(() => { ui.checked = { ok: r.ok, problems: r.problems ?? [], what: r.ok ? "Saved" : "Not saved" }; });
    };
    const remove = async () => { const r = await call(() => host.remove(`layouts/${l.id}.json`)); if (r?.ok) { take(r.files); ui.draft = null; ui.confirm = null; await recheck(); } draw(); };
    const copy = () => {
      const id = ui.copyId.trim();
      let file; try { file = JSON.parse(ui.draft.text); } catch { return show(() => { ui.checked = { ok: false, problems: [{ level: "error", path: "", code: "bad-json", message: "the text isn't JSON" }], what: "Not copied" }; }); }
      file.id = id;
      if (file.name && typeof file.name === "object") for (const k of Object.keys(file.name)) file.name[k] = `${file.name[k]} (copy)`;
      write(JSON.stringify(file, null, 2) + "\n");
    };
    const stockNow = l.stock && !l.modified;
    return [
      hint("A layout is its file, all of it. The stock ones are files like any other; a file of yours with a stock id replaces that one until you remove it."),
      h("div", { class: "searchbar" }, h("span", { class: "glass", "aria-hidden": "true" }, "⌕"), input, clear, count),
      h("div", { class: "scroll" }, h("table", {}, h("thead", {}, h("tr", {}, ["Layout", "It", "Used in", ""].map(t => h("th", {}, t)))), h("tbody", {}, rows, none))),
      card(l.name, `${l.id}.json`,
        row(pill(M.READS[l.read] ?? l.read, "on"), pill(`language ${l.lang}`), pill(l.kind === "muhenkan" ? "can be the muhenkan layout" : "for henkan profiles"),
          l.custom ? pill("no key model: ranked by its word lists alone", "warn") : null),
        l.forms.length ? h("div", { class: "forms" }, l.forms.map((f, n) => h("div", {}, h("i", {}, `−${n + 1}`), h("span", {}, f)))) : hint("It lists no forms."),
        l.actions.length ? hint("Actions, for F keys and bindings: ", l.actions.map(a => a.label).join(", "), ".") : null,
        l.stock ? banner(l.modified ? "This is your copy of a stock layout: it replaces the stock one. The models that rank it against other layouts still expect the stock keys."
          : "This is a stock layout. Saving a change makes a copy of yours that replaces it, and the models that rank it against other layouts still expect the stock keys.") : null,
        text,
        ui.checked ? h("div", { class: "stack tight" }, h("div", { class: ui.checked.ok ? "okline" : "errline" }, ui.checked.what, ui.checked.problems.length ? ":" : ui.checked.ok ? ": no problems." : "."),
          ui.checked.problems.map(p => h("div", { class: p.level === "error" ? "errline" : "warnline" }, h("b", {}, p.level), " ", p.path ? mono(p.path) : null, " ", p.code, ": ", p.message))) : null,
        row(h("button", { onclick: checkIt }, "Check"),
          ui.confirm === "stock" ? [h("span", { class: "muted" }, "Replace the stock layout with this?"), h("button", { class: "primary", onclick: () => write(ui.draft.text) }, "Save my copy"), h("button", { onclick: () => show(() => { ui.confirm = null; }) }, "Cancel")]
            : h("button", { class: "primary", onclick: () => (stockNow && draftId() === l.id ? show(() => { ui.confirm = "stock"; }) : write(ui.draft.text)) }, stockNow ? "Save as my copy…" : "Save"),
          ui.copyId == null ? h("button", { onclick: () => show(() => { ui.copyId = `${l.id}-copy`.slice(0, 48); }) }, "Duplicate as new…")
            : [h("input", { type: "text", class: "mono", value: ui.copyId, "aria-label": "The new layout's id", spellcheck: false, oninput: e => { ui.copyId = e.target.value; } }),
              h("button", { class: "primary", onclick: copy }, "Make it"), h("button", { onclick: () => show(() => { ui.copyId = null; }) }, "Cancel")],
          options.layoutEditor ? h("a", { class: "button", href: options.layoutEditor(l) }, "Edit field by field…") : null,
          grow(),
          l.custom ? h("button", { class: "danger", onclick: remove }, "Remove") : h("button", { disabled: !l.modified, onclick: remove }, "Reset to stock")),
        hint("An id is a group of two or three letters, then a name: ", mono("ja-kana-mine"), ". Saving under another id makes a new layout.")),
    ];
  }
  /** The settings may name a layout that came or went: asks the host what is wrong with them now. */
  async function recheck() {
    if (c.files.settings?.text == null) return;
    const r = await call(() => host.check({ settings: M.print(c.file) }));
    if (r) c.problems = r.problems ?? [];
  }

  // ---------- Dictionary ----------

  function dictionaryPage() {
    const d = c.dict, langs = [["korean", "Korean"], ["japanese", "Japanese"], ["english", "English"]];
    const saveIt = async () => {
      const kept = { ...d, positive: d.positive.filter(w => String(w.word ?? "").trim()), negative: d.negative.filter(w => String(w.word ?? "").trim()) };
      const r = await call(() => host.write("dictionary.json", M.printDictionary(kept)));
      if (r) { ui.checked = r.ok ? null : { ok: false, problems: r.problems ?? [], what: "Not saved" }; if (r.ok) { c.files = r.files ?? c.files; c.dictBad = false; } }
      drawStatus();
    };
    const edit = f => { f(); draw(); saveIt(); };
    if (c.dictBad) return [banner(mono("dictionary.json"), " isn't a ", mono("bada-dictionary"), " file, so it isn't loaded."),
      row(h("button", { class: "danger", onclick: () => edit(() => { c.dictBad = false; }) }, "Replace it with an empty dictionary"))];
    const words = (list, more) => h("div", { class: "scroll" }, h("table", {}, h("thead", {}, h("tr", {}, ["Word", "Language", ...more.map(m => m[0]), ""].map(t => h("th", {}, t)))),
      h("tbody", {}, list.map((w, i) => h("tr", {},
        h("td", {}, h("input", { type: "text", value: w.word ?? "", "aria-label": "Word", onchange: e => edit(() => { w.word = e.target.value.trim(); }) })),
        h("td", {}, select(langs, w.language ?? "korean", v => edit(() => { if (v === "korean") delete w.language; else w.language = v; }), { class: "small", "aria-label": "Language" })),
        more.map(m => h("td", {}, m[1](w))),
        h("td", {}, h("button", { class: "quiet", "aria-label": `Remove ${w.word ?? ""}`, onclick: () => edit(() => { list.splice(i, 1); }) }, "×")))))));
    return [
      hint("Your own words. A positive word ranks its reading up; a negative one ranks it down, or leaves it out. ", mono("dictionary.json"), ", ", mono("bada-dictionary"), " 1."),
      card("Words you mean", `${d.positive.length} positive`,
        d.positive.length ? words(d.positive, [["Counts as", w => [h("input", { type: "number", min: 1, value: w.count ?? "", placeholder: "50", "aria-label": "Count",
          onchange: e => edit(() => { const n = Math.floor(Number(e.target.value)); if (n >= 1) w.count = n; else delete w.count; }) }), h("span", { class: "muted" }, " uses")]]]) : hint("None yet."),
        row(h("button", { onclick: () => show(() => { d.positive.push({ word: "" }); }) }, "+ Add a word"))),
      card("Words you never mean", `${d.negative.length} negative`,
        d.negative.length ? words(d.negative, [["How hard", w => select([["soft", "Soft: far down the menu"], ["strong", "Strong: never offered"]], w.strength ?? "soft",
          v => edit(() => { if (v === "soft") delete w.strength; else w.strength = v; }), { class: "small", "aria-label": "Strength" })]]) : hint("None yet."),
        row(h("button", { onclick: () => show(() => { d.negative.push({ word: "" }); }) }, "+ Add a word"))),
      ui.checked && !ui.checked.ok ? h("div", { class: "errline" }, "Not saved: ", ui.checked.problems.map(p => `${p.path} ${p.code}: ${p.message}`).join("; ")) : null,
    ];
  }

  // ---------- Files ----------

  function filesPage() {
    const web = c.info.platform === "web", f = c.files;
    const files = [
      ["settings.json", `${M.FORMAT}: the engine's part, the keys, and each harness's own`, f.settings ? when(f.settings.modified) : "not written yet"],
      ...(f.layouts ?? []).map(l => [`layouts/${l.id}.json`, `${M.LAYOUT_FORMAT}: a layout of yours`, when(l.modified)]),
      ["dictionary.json", f.dictionary ? `${M.DICTIONARY_FORMAT} 1: ${c.dict.positive.length} positive, ${c.dict.negative.length} negative` : "no dictionary yet", f.dictionary ? when(f.dictionary.modified) : ""],
    ];
    const text = M.print(c.file);
    const exportIt = () => { const a = h("a", { href: URL.createObjectURL(new Blob([text], { type: "application/json" })), download: "bada-settings.json" }); a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 1000); };
    const copyIt = async () => { try { await navigator.clipboard.writeText(text); show(() => { ui.note = "Copied."; }); } catch { show(() => { ui.note = "The browser didn't allow the copy: use the settings.json panel and copy from there."; ui.json = true; }); } };
    const importIt = () => { const input = h("input", { type: "file", accept: ".json,application/json", onchange: async () => {
      const file = input.files[0]; if (!file) return;
      const r = await call(async () => host.write("settings.json", await file.text()));
      if (!r) return;
      if (r.ok) { ui.note = "Imported."; await reload(); } else show(() => { ui.note = `Not imported: ${(r.problems ?? []).filter(p => p.level === "error").map(p => `${p.path} ${p.code}`).join("; ")}`; });
    } }); input.click(); };
    const undo = async name => { const r = await call(() => host.undo(name)); if (r?.ok) { ui.note = "Back to that file."; await reload(); } else if (r) show(() => { ui.note = "That file can't be put back: it has an error."; }); };
    return [
      card("The folder", c.info.folder,
        h("div", { class: "scroll" }, h("table", {}, h("thead", {}, h("tr", {}, ["File", "Holds", "Changed"].map(t => h("th", {}, t)))),
          h("tbody", {}, files.map(x => h("tr", {}, h("td", { class: "mono" }, x[0]), h("td", {}, x[1]), h("td", { class: "muted" }, x[2])))))),
        hint(web ? "On the web the same files are kept in this browser, and Export and Import move the settings as a file."
          : "The input method watches this folder and reloads. This page and the bada command line only write files here, so settings can be changed while the input method isn't running.")),
      card("Move and undo", "",
        row(c.info.embedded && !web ? null : h("button", { onclick: exportIt }, "Export…"), h("button", { onclick: copyIt }, "Copy as JSON"), h("button", { onclick: importIt }, "Import…"),
          web ? null : h("button", { onclick: () => call(() => host.reveal()) }, "Show the folder"), grow(),
          ui.confirm === "reset" ? [h("span", { class: "muted" }, "Back to a new install?"), h("button", { class: "danger", onclick: () => { ui.confirm = null; c.file = M.fresh(); ui.profile = "@muhenkan"; draw(); save().then(() => first.open()); } }, "Start over"),
            h("button", { onclick: () => show(() => { ui.confirm = null; }) }, "Cancel")] : h("button", { class: "danger", onclick: () => show(() => { ui.confirm = "reset"; }) }, "Start over…")),
        ui.note ? h("p", { class: "muted" }, ui.note) : null,
        (f.history ?? []).length ? h("div", { class: "scroll" }, h("table", {}, h("thead", {}, h("tr", {}, ["history/", ""].map(t => h("th", {}, t)))),
          h("tbody", {}, f.history.map(x => h("tr", {}, h("td", {}, historyName(x.name)), h("td", { class: "right" }, h("button", { onclick: () => undo(x.name) }, "Go back to this"))))))) : null,
        hint(web ? "Importing replaces the whole file; a file with an error changes nothing, and says why above."
          : "Every save keeps the file it replaced, and the last 20 are kept. Importing replaces the whole file; a file with an error changes nothing, and says why above.")),
    ];
  }

  // ---------- the platform's own tab ----------

  function harnessPage() {
    const p = c.info.platform, seenKeys = keysSeen(), mine = own();
    const setOwn = f => change(file => { const o = (file.harness[c.info.harness] ??= {}); f(o); if (!Object.keys(o).length) delete file.harness[c.info.harness]; });
    const ask = what => { location.href = `bada-app:ask/${what}`; setTimeout(refreshState, 1500); };
    const permissions = [["Accessibility", "accessibility", "英数 and かな in every app, and bindings on other keys"],
      ["Input Monitoring", "input-monitoring", "無変換, 変換, カタカナ and Caps Lock, read from the keyboard itself"]];
    return [
      card(TAB_OF[p], c.info.harness,
        field("Keyboard", select([["auto", `Automatic (${M.PRESET_NAMES[keysSeen()?.preset ?? M.platformPreset(p)]})`], ["jis-mac", "JIS on a Mac"], ["jis-pc", "JIS on a PC"], ["none", "No language keys"]], mine.keyboard ?? "auto",
          v => setOwn(o => { if (v === "auto") delete o.keyboard; else o.keyboard = v; }), { "aria-label": "The keyboard the preset is for" })),
        hint("Which preset the keys get while the file has no bindings of its own. It changes nothing once it has."),
        p === "mac" || p === "windows" ? check(`Write a log (${p === "mac" ? "~/Library/Logs/Bada.log" : "%LOCALAPPDATA%\\bada\\bada.log"}): activations and errors, never what you type`, mine.log, v => setOwn(o => { if (v) o.log = true; else delete o.log; })) : null),
      p === "mac" ? card("What macOS allows", "System Settings → Privacy & Security",
        seenKeys ? permissions.map(([name, id, gives]) => { const on = !(seenKeys.missing ?? []).includes(name);
          return h("div", { class: "row top-aligned" }, h("div", { class: "grow" }, h("b", {}, name), " ", on ? pill("allowed", "on") : pill("not allowed", "warn"), h("p", { class: "hint" }, gives)),
            on ? null : c.info.embedded ? h("button", { onclick: () => ask(id) }, "Ask for it…") : null); })
          : hint("bada isn't running, so it can't say what it is allowed."),
        seenKeys && !c.info.embedded && (seenKeys.missing ?? []).length ? hint("Allow it in System Settings → Privacy & Security, under the permission's name.") : null)
        : card("What it sees", "", hint(p === "web" ? "The page gets every key the browser gives it. A keyboard with no language keys can click them on the drawn keyboard, or use ⇧Tab."
          : "The input method is handed the focused app's keys, modifiers included, so bindings on any key need no permission. A key the desktop takes first never arrives.")),
      card("Seen now", "harness.keys in the published state",
        seenKeys ? row((seenKeys.available ?? []).map(k => h("kbd", {}, M.KEY_NAMES[k] ?? k)), seenKeys.chords === false ? pill("no bindings on other keys", "warn") : pill("bindings on other keys", "on"))
          : hint("Nothing is published: bada isn't running, or no text field has had it yet."),
        row(h("button", { onclick: refreshState }, "Look again"))),
      card("Version", "",
        row(h("b", {}, `bada ${c.info.version || c.describe.engine?.version || ""}`), h("span", { class: "muted" }, [c.describe.engine ? `engine ${c.describe.engine.id} ${c.describe.engine.version}` : null,
          c.describe.abi ? `${c.describe.abi.name} ${c.describe.abi.major}.${c.describe.abi.minor}` : null, c.state?.harness?.version ? `running ${c.state.harness.version}` : null].filter(Boolean).join(" · "))),
        hint("bada never looks for updates by itself.")),
    ];
  }

  function aboutPage() {
    return [h("div", { class: "about" }, h("img", { src: new URL("icon.svg", import.meta.url).href, alt: "", width: 92, height: 92 }), h("h2", {}, "bada"), h("p", {}, "바다 · バダ"),
      h("p", { class: "muted" }, `Version ${c.info.version || c.describe.engine?.version || "?"}`),
      row(h("a", { href: "https://github.com/team-mirae/bada", target: "_blank", rel: "noopener" }, "GitHub"), h("a", { href: "https://bada.teammirae.com", target: "_blank", rel: "noopener" }, "Try it in a browser")),
      hint("An input method by Mirae Research for Korean, Japanese and English on JIS keyboards. MIT."))];
  }

  // ---------- drawing ----------

  function drawStatus() {
    const errors = c.problems.filter(p => p.level === "error").length;
    const sep = c.info.platform === "windows" ? "\\" : "/";
    status.replaceChildren(...[
      c.lost ? h("span", { class: "errline" }, h("span", { class: "dot bad" }), "The settings host stopped answering. Close this and open Settings again.")
        : h("span", {}, h("span", { class: `dot${c.problems.length ? " warn" : ""}` }), c.saving ? "Saving…" : !c.saved || errors ? "Not saved: the last good file stays in effect" : c.files.settings ? "Saved, and in effect" : "Nothing written yet"),
      c.info.folder ? mono(c.info.platform === "web" ? c.info.folder : `${c.info.folder}${sep}settings.json`) : null,
      h("span", {}, c.problems.length ? `${c.problems.length} problem${c.problems.length > 1 ? "s" : ""}` : "No problems"),
      c.unreadable ? banner(mono("settings.json"), " can't be read, so this is a new file. Your first change replaces it; the old one is kept in history (Files tab).") : null,
      c.problems.length ? h("div", { class: "problems" }, c.problems.map(p => h("div", {}, h("b", {}, p.level), " ", p.path ? mono(p.path) : null, " ", p.code, ": ", p.message))) : null,
    ].filter(Boolean));
  }
  function drawPage() {
    const names = ["Profiles", "Keys", "Layouts", "Dictionary", "Files", TAB_OF[c.info.platform], "About"];
    if (!names.includes(ui.tab)) ui.tab = "Profiles";
    tabs.replaceChildren(...names.map(t => h("button", { role: "tab", "aria-selected": String(t === ui.tab), onclick: () => { ui.tab = t; ui.editKey = null; ui.recording = false; ui.confirm = null; ui.note = ""; ui.checked = null;
      if (t === "Keys" || t === TAB_OF[c.info.platform]) refreshState(); else draw(); } }, t)));
    const made = ui.tab === "Profiles" ? profilesPage() : ui.tab === "Keys" ? keysPage() : ui.tab === "Layouts" ? layoutsPage() : ui.tab === "Dictionary" ? dictionaryPage()
      : ui.tab === "Files" ? filesPage() : ui.tab === "About" ? aboutPage() : harnessPage();
    page.replaceChildren(...made.flat().filter(Boolean));
    body.className = `body${ui.json ? " with-json" : ""}`;
    json.hidden = !ui.json;
    jsonText.textContent = M.print(c.file);
  }
  function draw() {
    top.replaceChildren(h("div", { class: "brand" }, h("img", { src: new URL("icon.svg", import.meta.url).href, alt: "", width: 26, height: 26 }), h("b", {}, "bada"), h("span", { class: "muted" }, "Settings")), grow(),
      h("button", { "aria-pressed": String(ui.json), onclick: () => show(() => { ui.json = !ui.json; }) }, "settings.json"));
    drawStatus();
    drawPage();
  }

  draw();
  await reload();
  return { reload, showJson: on => show(() => { ui.json = on; }) };
}
