// The page's settings dialog and its onboarding (the standard, §8.3). The dialog holds the shared
// settings page (settings/), with this harness as its host (settings-host.js): the folder is this
// browser's local storage. All of it goes through harness.js.

import { mount } from "./settings/page.js";
import { webHost } from "./settings-host.js";

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

// ── The dialogs ────────────────────────────────────────────────────────────────────────────────

let page = null;

export function openSettings(ui) {
  const dialog = $("settings");
  ui.h.drop();   // no key is down for the key table any more
  // The page is drawn once, and reads the folder again each time the dialog opens. The harness
  // has its own first run (below), so the page's is left out.
  if (page) page.then(p => p.reload());
  else page = mount($("settingsPage"), webHost(() => ui.h, () => ui.stock), { header: false, firstRun: false,
    layoutEditor: l => `layout.html?from=${l.stock && !l.modified ? "stock" : "mine"}:${encodeURIComponent(l.id)}` });
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
