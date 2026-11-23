// The page's settings dialog and its onboarding (the standard, §8.3). The shared settings page
// doesn't exist yet, so this is plain: a JSON editor for the settings file, for a layout and for
// the dictionary. Each shows every problem the engine or the harness finds, and nothing is taken
// unless it is right. All of it goes through harness.js.

import * as S from "./settings.js";

const $ = id => document.getElementById(id);

function make(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e[k] = v;
    else if (v === true) e.setAttribute(k, "");
    else if (v !== false && v != null) e.setAttribute(k, v);
  }
  e.append(...children.filter(c => c != null));
  return e;
}

/** The problems of a check, as the engine and the harness report them (§6.6): where, what, and why. */
function listProblems(host, problems) {
  host.replaceChildren(...problems.map(p => make("li", { class: p.level === "error" ? "error" : "warning" },
    make("code", {}, p.path || "(the whole)"), ` ${p.level}, ${p.code}: ${p.message}`)));
}

function say(host, ok, text) {
  host.className = `verdict ${ok ? "ok" : "error"}`;
  host.textContent = text;
}

/** Reads a file the user picks into `then(text)`. */
function openFile(then) {
  const input = make("input", { type: "file", accept: ".json,application/json" });
  input.onchange = async () => { if (input.files[0]) then(await input.files[0].text()); };
  input.click();
}

function download(name, text) {
  const url = URL.createObjectURL(new Blob([text], { type: "application/json" }));
  make("a", { href: url, download: name }).click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

const editor = (id, placeholder) => make("textarea", { class: "code", id, spellcheck: "false", autocomplete: "off", autocapitalize: "off", placeholder });

// ── The settings file ──────────────────────────────────────────────────────────────────────────

function settingsPage(ui) {
  const h = ui.h;
  const text = editor("settingsText", "A bada-settings/2 file");
  const verdict = make("div", { class: "verdict" }), problems = make("ul", { class: "problems" });
  const effective = make("pre");
  const show = () => { effective.textContent = JSON.stringify(h.effective, null, 2); };
  text.value = h.settingsText;
  listProblems(problems, h.problems.settings);
  if (h.broken) say(verdict, false, "What this browser keeps can't be used. Nothing was changed: the engine has its defaults.");
  show();

  const save = () => {
    const result = h.loadSettings(text.value);
    say(verdict, result.ok, result.ok ? "Saved." : "Not saved: nothing changed.");
    listProblems(problems, result.problems);
    show();
    ui.refresh();
  };
  const check = () => {
    const result = h.loadSettings(text.value, false, true);
    say(verdict, result.ok, result.ok ? (result.problems.length ? "It can be saved, with these warnings." : "Nothing is wrong with it.") : "It can't be saved.");
    listProblems(problems, result.problems);
  };
  return make("div", { class: "page" },
    make("p", { class: "hint" }, "One ", make("code", {}, S.FORMAT), " file, kept in this browser. ", make("code", {}, "engine"),
      " goes to the engine, ", make("code", {}, "keys"), " is the key table, and ", make("code", {}, `harness["${S.WEB}"]`),
      " is this page's own. Export it to move it to another bada; nothing is taken unless all of it is right."),
    text,
    make("div", { class: "row" },
      make("button", { class: "primary", onclick: save }, "Save"),
      make("button", { onclick: check }, "Check"),
      make("button", { onclick: () => { text.value = h.settingsText; verdict.textContent = ""; listProblems(problems, []); } }, "Revert"),
      make("button", { onclick: () => openFile(file => { text.value = file; save(); }) }, "Import a file…"),
      make("button", { onclick: () => download("settings.json", h.settingsText) }, "Export"),
      make("button", { onclick: () => { text.value = S.print(S.fresh(h.engine.describe)); say(verdict, true, "A new install's settings. Not saved yet."); listProblems(problems, []); } }, "A new install's")),
    verdict, problems,
    make("details", {}, make("summary", {}, "The engine's part in effect"), effective));
}

// ── Layouts (§7.6) ─────────────────────────────────────────────────────────────────────────────

function layoutsPage(ui) {
  const h = ui.h;
  const text = editor("layoutText", "A bada-layout/2 file: a new layout, or a changed copy of one with the same id");
  const verdict = make("div", { class: "verdict" }), problems = make("ul", { class: "problems" });
  const table = make("table", { class: "list" });
  let warned = null;   // the text whose warnings were shown: saving it again takes it

  const edit = value => { text.value = JSON.stringify(value, null, 2); verdict.textContent = ""; listProblems(problems, []); text.focus(); };
  const remove = id => {
    const result = h.removeLayout(id);
    say(verdict, result.ok, result.ok ? `${id} was removed.` : `${id} wasn't removed.`);
    listProblems(problems, result.problems);
    draw();
    ui.refresh();
  };
  function draw() {
    const kept = h.customLayouts;
    const rows = h.layouts.map(l => {
      const tags = [make("span", { class: "tag" }, l.stock ? "stock" : "yours")];
      if (l.modified) tags.push(" ", make("span", { class: "tag warn" }, "modified"));
      const mine = kept[l.id];
      return make("tr", {},
        make("td", {}, make("code", {}, l.id)),
        make("td", {}, l.name?.[ui.language] ?? l.name?.en ?? ""),
        make("td", {}, `${l.kind}, ${l.read}`),
        make("td", {}, ...tags),
        make("td", {}, mine
          ? make("span", { class: "row" }, make("button", { onclick: () => edit(mine) }, "Edit"), make("button", { onclick: () => remove(l.id) }, l.stock ? "Back to stock" : "Remove"))
          : ui.stock[l.id] ? make("button", { onclick: () => edit(ui.stock[l.id]) }, "Edit a copy") : null));
    });
    // A layout in storage that the engine didn't take is still the user's: it is listed, with why.
    for (const [id, value] of Object.entries(kept)) {
      if (h.layouts.some(l => l.id === id && (l.modified || !l.stock))) continue;
      rows.push(make("tr", {},
        make("td", {}, make("code", {}, id)), make("td", {}, value?.name?.en ?? ""), make("td", {}, ""),
        make("td", {}, make("span", { class: "tag warn" }, "not loaded")),
        make("td", {}, make("span", { class: "row" }, make("button", { onclick: () => { edit(value); listProblems(problems, h.problems.layouts[id] ?? []); } }, "Edit"), make("button", { onclick: () => remove(id) }, "Remove")))));
    }
    table.replaceChildren(...rows);
  }
  const save = () => {
    // A dry run first. Its warnings are shown before anything is saved (§7.7): a changed stock
    // layout keeps its key model, and a new id has none.
    const tried = h.loadLayout(text.value, true);
    listProblems(problems, tried.problems);
    if (!tried.ok) return say(verdict, false, "Not saved: nothing changed.");
    if (tried.problems.length && warned !== text.value) {
      warned = text.value;
      return say(verdict, false, "Not saved yet. Read the warnings, then Save again to take it.");
    }
    const result = h.loadLayout(text.value);
    say(verdict, result.ok, result.ok ? `${result.id} was saved.` : "Not saved: nothing changed.");
    listProblems(problems, result.problems);
    draw();
    ui.refresh();
  };
  const check = () => {
    const tried = h.loadLayout(text.value, true);
    say(verdict, tried.ok, tried.ok ? (tried.problems.length ? "It can be saved, with these warnings." : "Nothing is wrong with it.") : "It can't be saved.");
    listProblems(problems, tried.problems);
  };
  draw();
  return make("div", { class: "page" },
    make("p", { class: "hint" }, "The session's layouts. A layout you save here is kept in this browser and fed to the engine; one with a stock id replaces the stock layout until it is removed."),
    table, text,
    make("div", { class: "row" },
      make("button", { class: "primary", onclick: save }, "Save"),
      make("button", { onclick: check }, "Check"),
      make("button", { onclick: () => openFile(file => { text.value = file; check(); }) }, "Open a file…")),
    verdict, problems);
}

// ── The dictionary (§11) ───────────────────────────────────────────────────────────────────────

function dictionaryPage(ui) {
  const h = ui.h;
  const text = editor("dictionaryText", '{"format": "bada-dictionary", "version": 1, "positive": [], "negative": []}');
  const verdict = make("div", { class: "verdict" }), problems = make("ul", { class: "problems" });
  const summary = make("p", { class: "hint" });
  const show = () => { summary.textContent = `The engine has ${h.dictionary.positive} positive and ${h.dictionary.negative} negative entries.`; };
  const load = value => {
    const result = h.loadDictionary(value);
    say(verdict, result.ok, result.ok ? (value.trim() ? "Saved." : "Cleared.") : "Not saved: nothing changed.");
    listProblems(problems, result.problems);
    show();
    ui.refresh();
  };
  text.value = h.dictionaryText;
  listProblems(problems, h.problems.dictionary);
  show();
  return make("div", { class: "page" },
    make("p", { class: "hint" }, "The user dictionary: a ", make("code", {}, "bada-dictionary"), " file, version 1, kept in this browser and fed to the engine."),
    text,
    make("div", { class: "row" },
      make("button", { class: "primary", onclick: () => load(text.value) }, "Save"),
      make("button", { onclick: () => { text.value = ""; load(""); } }, "Clear"),
      make("button", { onclick: () => openFile(file => { text.value = file; load(file); }) }, "Open a file…")),
    summary, verdict, problems);
}

// ── The dialogs ────────────────────────────────────────────────────────────────────────────────

const PAGES = [["Settings", settingsPage], ["Layouts", layoutsPage], ["Dictionary", dictionaryPage]];

export function openSettings(ui, tab = "Settings") {
  const dialog = $("settings");
  ui.h.drop();   // no key is down for the key table any more
  const show = name => {
    $("tabs").replaceChildren(...PAGES.map(([title]) => make("button", { role: "tab", type: "button", "aria-selected": String(title === name), onclick: () => show(title) }, title)));
    $("pages").replaceChildren(PAGES.find(([title]) => title === name)[1](ui));
  };
  show(tab);
  dialog.onclose = () => { ui.refresh(); ui.focus(); };
  if (!dialog.open) dialog.showModal();
}

/** The onboarding (§8.3). Easy mode: the muhenkan layout and one henkan layout. Skip: the full settings. */
export function openOnboarding(ui) {
  const h = ui.h, dialog = $("onboarding");
  h.drop();
  const name = l => l.name?.[ui.language] ?? l.name?.en ?? l.id;
  const fill = (select, layouts, first) => {
    select.replaceChildren(...layouts.map(l => make("option", { value: l.id }, `${name(l)} (${l.id})`)));
    if (layouts.some(l => l.id === first)) select.value = first;
  };
  // Only a layout that types directly can be the muhenkan layout; either kind can be a henkan candidate (§7.5).
  fill($("easyMuhenkan"), h.layouts.filter(l => l.kind === "muhenkan"), "en-qwerty");
  fill($("easyHenkan"), [...h.layouts.filter(l => l.kind === "henkan"), ...h.layouts.filter(l => l.kind !== "henkan")], ui.language === "ko" ? "ko-2-set" : "ja-romaji");
  const problems = make("ul", { class: "problems" });
  $("easyProblems").replaceChildren(problems);
  $("easyStart").onclick = () => {
    const result = h.easy($("easyMuhenkan").value, $("easyHenkan").value);
    if (!result.ok) return listProblems(problems, result.problems);
    dialog.close();
  };
  $("easySkip").onclick = () => { dialog.close(); openSettings(ui); };
  dialog.onclose = () => { ui.refresh(); ui.focus(); };
  if (!dialog.open) dialog.showModal();
}
