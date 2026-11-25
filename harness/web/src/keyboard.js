// The keyboard drawn on the page: a board of keyboards.js with the legends of the layouts in use,
// read from their bada-layout/3 files. Keys can be clicked (app.js).

import { JIS, KEYBOARDS } from "./keyboards.js";

const NS = "http://www.w3.org/2000/svg";
const PAD = 14, G = 3, R = 7;
const el = (tag, attrs, text) => {
  const e = document.createElementNS(NS, tag);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  if (text != null) e.textContent = text;
  return e;
};

/** A key's slot in a layout, `{t, r}` or null: the layout's own, or the JIS table's where it has none (§7.5.1). */
function slot(layout, code, layer) {
  const key = layout.keys?.[code];
  if (key && layer in key) return key[layer];
  const legend = JIS[code]?.[layer === "base" ? 0 : 1];
  return legend ? { t: legend, r: code.startsWith("Key") ? "l" : "x" } : null;
}

// Several layouts on one keycap: each takes a corner by its language, the next free one if it is taken.
const corners = lang => (lang === "ja" ? ["BR", "BL", "TL"] : lang === "ko" ? ["BL", "BR", "TL"] : ["TL", "BR", "BL"]);
const norm = t => t.normalize("NFKC").toLowerCase();

function rect(x, y, w, h) {
  const r = Math.min(R, w / 2, h / 2);
  return `M${x + r},${y}H${x + w - r}Q${x + w},${y} ${x + w},${y + r}V${y + h - r}Q${x + w},${y + h} ${x + w - r},${y + h}H${x + r}Q${x},${y + h} ${x},${y + h - r}V${y + r}Q${x},${y} ${x + r},${y}Z`;
}
function enter(x, y, U) {   // 1.5u on top, 1.25u below
  const x0 = x + G, x1 = x + 0.25 * U + G, x2 = x + 1.5 * U - G, y0 = y + G, ym = y + U - G, y2 = y + 2 * U - G;
  return `M${x0 + R},${y0}H${x2 - R}Q${x2},${y0} ${x2},${y0 + R}V${y2 - R}Q${x2},${y2} ${x2 - R},${y2}H${x1 + R}Q${x1},${y2} ${x1},${y2 - R}V${ym}H${x0 + R}Q${x0},${ym} ${x0},${ym - R}V${y0 + R}Q${x0},${y0} ${x0 + R},${y0}Z`;
}

/**
 * Draws board `name` with `layouts` (bada-layout/3 objects, in candidate order: three at most show)
 * into `host`. `bound`: the codes the key table has a binding on, which are outlined.
 * Returns code → key group.
 */
export function drawKeyboard(host, { name, layouts, bound = [] }) {
  const kb = KEYBOARDS[name] ?? KEYBOARDS.hhkb;
  const layers = layouts.slice(0, 3);
  const multi = layers.length > 1;
  const U = multi ? 80 : 60;
  const W = kb.width * U + PAD * 2, H = kb.height * U + PAD * 2;
  const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, xmlns: NS, role: "img",
    "aria-label": `${kb.name}: ${layers.map(l => l.name?.en ?? l.id).join(" + ") || "no layout"}` });
  svg.append(el("rect", { x: 0.5, y: 0.5, width: W - 1, height: H - 1, rx: 14, class: "case" }));
  const used = new Set();
  const slots = layers.map(l => { const s = corners(l.lang).find(c => !used.has(c)) ?? "TL"; used.add(s); return s; });
  const byCode = {};
  for (const [code, kx, ky, kw, kh, label, shape] of kb.keys) {
    const x = PAD + kx * U, y = PAD + ky * U, w = kw * U, h = kh * U;
    const typing = !!JIS[code] && layers.length > 0;
    const g = el("g", { class: "key" + (typing ? "" : " mod") + (bound.includes(code) ? " lang" : "") });
    if (code) g.dataset.code = code;
    g.append(el("path", { d: shape === "ENTER" ? enter(x, y, U) : rect(x + G, y + G, w - 2 * G, h - 2 * G), class: "cap" }));
    const text = (t, lx, ly, size, role, side, anchor = "start") => {
      g.append(el("text", { x: lx, y: ly, "font-size": size, "text-anchor": anchor, class: `lg ${side} c-${role}` }, t));
    };
    if (typing && multi) {
      // Each layout in its corner. A legend another layout already shows isn't drawn twice.
      const shown = new Set();
      const say = (s, lx, ly, size, side, anchor) => { text(s.t, lx, ly, size, s.r, side, anchor); shown.add(norm(s.t)); };
      layers.forEach((layout, i) => {
        let base = slot(layout, code, "base"), shift = slot(layout, code, "shift");
        if (base && shown.has(norm(base.t))) base = null;
        if (shift && shown.has(norm(shift.t))) shift = null;
        if (base && shift && norm(base.t) === norm(shift.t)) shift = null;
        if (slots[i] === "TL") {
          if (code.startsWith("Key") && base && /^[a-z]$/i.test(base.t)) say({ t: base.t.toUpperCase(), r: base.r }, x + 11, y + 26, 18, "base");
          else {
            if (shift) say(shift, x + 11, y + 23, 13, "shift");
            if (base) say(base, x + 11, y + 41, 13, "base");
          }
        } else if (slots[i] === "BL") {
          if (base) say(base, x + 11, y + h - 13, base.t.length > 2 ? 15 : 23, "base");
          if (shift) say(shift, x + 38, y + h - 27, 12, "shift");
        } else {
          if (base) say(base, x + w - 11, y + h - 13, base.t.length > 2 ? 12 : 18, "base", "end");
          if (shift) say(shift, x + w - 11, y + h - 36, 12, "shift", "end");
        }
      });
    } else if (typing) {
      const base = slot(layers[0], code, "base"), shift = slot(layers[0], code, "shift");
      const letter = base && shift && /^[a-z]$/.test(base.t) && shift.t === base.t.toUpperCase();
      if (letter) text(shift.t, x + 12, y + 30, 22, shift.r, "base");
      else {
        if (base) text(base.t, x + 12, y + h - 13, base.t.length > 2 ? 15 : 24, base.r, "base");
        if (shift && (!base || shift.t !== base.t)) text(shift.t, x + 12, y + 24, shift.t.length > 2 ? 11 : 15, shift.r, "shift");
      }
    } else if (label != null) {
      const lines = label.split("/");
      const size = (h < U ? 12 : lines.length > 1 || label.length > 4 ? 11 : 14) * (multi ? 1.2 : 1);
      const cy = shape === "ENTER" ? y + U / 2 : y + h / 2;
      const cx = shape === "ENTER" ? x + 0.75 * U : x + w / 2;
      lines.forEach((line, i) => g.append(el("text", { x: cx, y: cy + 4 + (i - (lines.length - 1) / 2) * 13, "font-size": size, "text-anchor": "middle", class: "modl" }, line)));
    }
    svg.append(g);
    if (code) byCode[code] = g;
  }
  host.replaceChildren(svg);
  return byCode;
}
