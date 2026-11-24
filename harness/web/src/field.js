// The page's text field (the standard, §6.3): a text, a selection (a caret when empty) and an
// optional marked range, with what shows over them: the preedit's segments and the menu.
// A pure model: no DOM, no engine. The page mirrors it into a <textarea> (app.js), and the tests
// read it as it is.
//
// Offsets here are UTF-16 code units, as a <textarea>'s are. The engine counts in Unicode scalar
// values (§3.4): they are converted where a reply is applied.

const NO_MENU = Object.freeze({ rows: Object.freeze([]), selected: null, strip: false });

/** The UTF-16 offset of the `n`th Unicode scalar value of `text`. */
function offsetOf(text, n) {
  let at = 0;
  for (const c of text) {
    if (n-- <= 0) break;
    at += c.length;
  }
  return at;
}

const scalars = text => Array.from(text);

export class Field {
  constructor() {
    this.text = "";
    /** The selection: `start` to `end`, and a caret when they are equal. */
    this.start = 0;
    this.end = 0;
    /** The marked range, `{start, len}`, or null. */
    this.marked = null;
    /** The marked text's parts, `[{start, len, kind}]` from the field's start: live, fixed or picked. */
    this.segments = [];
    this.menu = NO_MENU;
    this.dir = "ltr";
  }

  get selected() {
    return this.text.slice(this.start, this.end);
  }

  get markedText() {
    return this.marked ? this.text.slice(this.marked.start, this.marked.start + this.marked.len) : "";
  }

  /** Replaces `start`..`end` with `text` and leaves the caret after it. */
  replace(start, end, text) {
    this.text = this.text.slice(0, start) + text + this.text.slice(end);
    this.start = this.end = start + text.length;
  }

  /** Deletes the marked text and closes the menu. */
  unmark() {
    if (this.marked) this.replace(this.marked.start, this.marked.start + this.marked.len, "");
    this.marked = null;
    this.segments = [];
  }

  /**
   * Applies one reply, whatever the event was (§6.3). `send` passes an event to the engine and
   * returns its reply: it is used for the one `text` event that answers an `ask`, never deeper.
   */
  apply(reply, send = null, answering = false) {
    if (!reply.ok) {
      // Nothing changed. After a panic nothing is staged either: the marked text and the menu go.
      if (reply.error?.code === "panic") {
        this.unmark();
        this.menu = NO_MENU;
      }
      return;
    }
    const commit = reply.commit ?? "";
    if (commit !== "") {
      const m = this.marked;
      const [start, end] = reply.replace === "selection" || !m ? [this.start, this.end] : [m.start, m.start + m.len];
      this.replace(start, end, commit);
      this.marked = null;
    }
    const preedit = reply.preedit?.text ?? "";
    if (preedit !== "") {
      const m = this.marked;
      const [start, end] = m ? [m.start, m.start + m.len] : [this.start, this.end];
      this.replace(start, end, preedit);
      this.marked = { start, len: preedit.length };
      const caret = reply.preedit.caret;
      this.start = this.end = start + (caret == null ? preedit.length : offsetOf(preedit, caret));
      this.segments = segmentsOf(reply.preedit, start);
    } else {
      this.unmark();
    }
    const menu = reply.menu ?? {};
    this.menu = { rows: menu.rows ?? [], selected: menu.selected ?? null, strip: !!menu.strip };
    this.dir = reply.dir === "rtl" ? "rtl" : "ltr";
    if (reply.ask && !answering && send) this.apply(send(this.answer(reply.ask)), send, true);
  }

  /** The `text` event that answers an `ask` (§5): the selection, or the text around the caret. */
  answer(ask) {
    const event = { type: "text" };
    const m = this.marked;
    if (typeof ask.selection === "string") event.selection = m ? "" : this.selected;
    if (ask.around) {
      const [start, end] = m ? [m.start, m.start + m.len] : [this.start, this.end];
      const before = scalars(this.text.slice(0, start)), after = scalars(this.text.slice(end));
      event.before = ask.around.before > 0 ? before.slice(-ask.around.before).join("") : "";
      event.after = after.slice(0, ask.around.after ?? 0).join("");
    }
    return event;
  }

  // ---------- the app's side: what a plain text field does ----------

  /** Types text over the selection. */
  insert(text) {
    this.replace(this.start, this.end, text);
  }

  /** The view changed under the model (the browser edited its own field). Nothing is marked then. */
  set(text, start, end) {
    this.text = text;
    this.start = Math.min(start, text.length);
    this.end = Math.min(Math.max(end, this.start), text.length);
    if (this.marked && this.marked.start + this.marked.len > text.length) this.marked = null;
  }

  /**
   * A key the engine passed on that a browser isn't there to handle (a click on the keyboard drawn
   * on the page, a test): the least a text field does with it. Returns whether it did anything.
   */
  key(code, { shift = false } = {}) {
    const { text, start, end } = this;
    const before = () => (start > 0 ? start - scalars(text.slice(0, start)).pop().length : 0);
    const after = () => (end < text.length ? end + scalars(text.slice(end))[0].length : end);
    const move = (to, anchor) => { [this.start, this.end] = shift ? [Math.min(to, anchor), Math.max(to, anchor)] : [to, to]; };
    switch (code) {
      case "Backspace": this.replace(start === end ? before() : start, end, ""); return true;
      case "Delete": this.replace(start, start === end ? after() : end, ""); return true;
      case "ArrowLeft": move(start === end || shift ? before() : start, end); return true;
      case "ArrowRight": move(start === end || shift ? after() : end, start); return true;
      case "Home": case "ArrowUp": move(0, end); return true;
      case "End": case "ArrowDown": move(text.length, start); return true;
    }
    return false;
  }
}

/** A preedit's segments as ranges of the field. Lengths that don't add up to the text: one live part. */
function segmentsOf(preedit, start) {
  const text = preedit.text;
  const parts = [];
  let at = 0, n = 0;
  for (const s of preedit.segments ?? []) {
    const from = offsetOf(text, n), to = offsetOf(text, n + s.len);
    parts.push({ start: start + from, len: to - from, kind: s.kind });
    n += s.len;
    at = to;
  }
  if (!parts.length || at !== text.length || n !== scalars(text).length) return [{ start, len: text.length, kind: "live" }];
  return parts;
}
