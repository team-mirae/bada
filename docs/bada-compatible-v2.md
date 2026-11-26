# bada-compatible v2

The interface between a **bada-compatible engine** and its **harness**. The *engine* turns keys into
text. A *harness* is the platform program around it: it gets keys from the OS, feeds the engine,
puts the text in the app and draws the menu (v1 called it the host, wrapper or shell). bada's own
harnesses are its macOS, Windows, Linux and web input methods and its command line.

v2 is the standard of **bada**, the input method of `team-mirae/bada`. It grows from
[bada-compatible v1](reference/bada-compatible-v1.md), which stays as it is for bada ime and its
ports. Nothing written for v1 loads a v2 engine, or the other way round: every function changed.

The words **must**, **should** and **may** are used as in RFC 2119. "As v1" marks a rule that v2
keeps unchanged from v1, with v1's section. The JSON Schemas are in [schemas/](schemas/), with
examples; `uv run tools/validate/validate.py` checks them, the stock layouts and the cases.

- [1. Versions](#1-versions)
- [2. Shape](#2-shape)
- [3. Functions](#3-functions)
- [4. Key codes](#4-key-codes)
- [5. Events](#5-events)
- [6. Replies](#6-replies)
- [7. Layouts](#7-layouts)
- [8. Henkan profiles](#8-henkan-profiles)
- [9. Keys the harness decides](#9-keys-the-harness-decides)
- [10. Settings files and the command line](#10-settings-files-and-the-command-line)
- [11. Data and dictionary](#11-data-and-dictionary)
- [12. Identifiers](#12-identifiers)
- [13. Conformance](#13-conformance)
- [14. Where bada is more specific](#14-where-bada-is-more-specific)
- [15. From v1](#15-from-v1)

## 1. Versions

- **v2.0**: this document. An engine reports it as `{"name": "bada-compatible", "major": 2, "minor": 0}`
  (§3.2).
- **A new major version** is a changed function, a changed meaning of a field, or a removed one. A
  harness **must** refuse an engine with another `major`.
- **A new minor version** adds things a v2.0 reader can ignore: an event type, a reply field, a
  feature, a part of a layout file. An engine says what it has in `P_describe`, and a harness says
  what it can do in `P_session_new`; neither probes the other (no `dlsym`).
- **The engine's and the harness's own versions are theirs**, not the standard's: `engine.version`
  and `harness.version`. bada has one version number for everything in its repository (the engine,
  the command line, every harness), in the `VERSION` file at its root; a platform that is released
  less often skips numbers.
- **File formats carry their own names**: `bada-settings/2` (§8, §9, §10.1), `bada-layout/3` (§7.5),
  `bada-compatible-state/2` (§6.7), `bada-key-cases/1` (§9.6). The user dictionary is still
  `bada-dictionary`, version 1 (§11).

## 2. Shape

As v1 (§2), but for the modes' names:

- The engine is a **library with a C ABI**: a shared library for harnesses that load it at run
  time, a static library for one that links it.
- Every function is named `<prefix>_<name>`. The prefix is the engine's own (`bada`, `shout`), so
  one library can hold several engines.
- Structured values cross as **UTF-8 JSON** in NUL-terminated strings: events in, replies out. The
  only binary arguments are the buffers of `P_data_new`.
- **Event in → reply out, no I/O.** The engine doesn't read files, open windows or look at the
  clock. The harness loads data, reads the settings and layout files, passes keys in, and applies
  each reply to the text field.
- **Two modes**, named by strings: `"muhenkan"` (無変換, 무변환: keys type directly or pass to the
  app) and `"henkan"` (変換, 변환: keys compose into a preedit, which is committed).

Two rules that v1 only implied:

- **The engine only sees what its harness feeds it.** It gets keys to read and requests by name. It
  never learns which key the user pressed to change mode or profile, what app has focus, or whether
  Caps Lock is on. The harness decides what language keys and Caps Lock do (§9).
- **A reply only ever answers an event.** No timers, no idle events, nothing re-ranked while the
  user pauses.

## 3. Functions

`P` is the prefix. All eight are required; everything else an engine does is an event type (§5).

```c
typedef struct { const char *name; const uint8_t *ptr; size_t len; uint32_t flags; } PBuf;
#define P_BORROWED 1u   /* the buffer outlives the Data: the engine may keep pointers into it */

const char *P_describe(void);                                  /* static JSON; no engine needed  */

PData      *P_data_new(const PBuf *bufs, size_t n);            /* immutable, shared, thread-safe */
void        P_data_free(PData *data);

PSession   *P_session_new(const PData *data, const char *init_json);   /* settings, layouts, user dictionary */
void        P_session_free(PSession *session);

PEngine    *P_new(PSession *session);                          /* one text field                 */
void        P_free(PEngine *engine);

const char *P_send(PSession *session, PEngine *engine_or_null, const char *event_json);   /* → reply JSON */
```

| Function | Returns |
|---|---|
| `P_describe` | What the engine is and has (§3.2). The same string every time, before anything is loaded |
| `P_data_new` | The data, from **named buffers** (§11). NULL when a buffer the engine requires is missing or can't be read. A name the engine doesn't know is ignored; a missing optional buffer turns its feature off |
| `P_data_free` | Frees the data, after every session on it is freed. NULL: no-op |
| `P_session_new` | A session on `data` (§3.3). NULL when `init_json` isn't a JSON object with `harness`. `data` may be NULL for an engine that needs none |
| `P_session_free` | Frees the session, after every engine of it is freed. NULL: no-op |
| `P_new` | A new engine: one text field. NULL on failure |
| `P_free` | Frees the engine. NULL: no-op |
| `P_send` | The reply to one event (§5, §6). Never NULL for a session that isn't |

### 3.1 Three objects

- **Data**: dictionaries and models, built once, immutable, shareable between threads.
- **Session**: what the harness has fed that every text field shares: the settings, the custom
  layouts and the user dictionary. One per process in an input method, one per run of the command
  line.
- **Engine**: one text field: its mode, its profile, its staged keys.

**Mode and profile are the harness's.** It tells an engine with one `select` event (§5): after
`P_new`, and when they changed while another field had focus. The settings are the session's, so
they are never sent again per field.

`engine_or_null`: the `settings`, `layout` and `dictionary` events are the session's and need no
engine; an engine passed with one is ignored. Every other event is about one text field and answers
`needs-engine` without one.

### 3.2 `P_describe`

```json
{"abi": {"name": "bada-compatible", "major": 2, "minor": 0},
 "engine": {"id": "com.teammirae.bada", "version": "0.1.0", "name": {"en": "bada", "ko": "바다", "ja": "バダ"}},
 "events": ["key", "action", "select", "pick", "text", "flush", "reset", "state", "settings", "layout", "dictionary"],
 "features": ["segments", "around", "custom-layouts"],
 "keyboards": ["jis"],
 "data": [{"name": "ko-words", "required": false}, {"name": "kanji", "required": false, "borrow": true}],
 "layouts": [{"id": "ja-kana", "name": {"en": "Japanese - Kana"}, "kind": "henkan", "read": "kana", "lang": "ja",
              "actions": [{"id": "katakana", "label": {"en": "Katakana"}}], "options": {},
              "stock": true, "modified": false}],
 "actions": ["menu.next", "menu.prev", "fix", "width.full", "width.half"],
 "settings": {"format": "bada-settings/2", "schema": {}, "defaults": {}},
 "limits": {"profiles": 32}}
```

| Field | Meaning |
|---|---|
| `abi` | The standard's name and version (§1) |
| `engine.id`, `.version`, `.name` | The engine's id (§12), its own version, and its name by language |
| `events` | The event types it takes. `key`, `select`, `flush`, `reset` and `state` are required; any other type it lacks answers `unknown-event` |
| `features` | What it writes or takes beyond the required: `segments` (it cuts the preedit into segments), `around` (it may ask for the text around the caret), `custom-layouts` (it takes `layout` events), `rtl` (it may write `dir: "rtl"`) |
| `keyboards` | The keyboards its layouts are for. v2.0 has `jis` only (§4) |
| `data` | The named buffers it knows: `required`, and `borrow` when it can use the buffer in place (§11) |
| `layouts` | The layouts shipped with it: `id`, `name`, `kind`, `read`, `lang`, their `actions` and `options` as their files declare them (§7.5). `stock` is true and `modified` false here: a session's own list is in the reply to a `layout` event (§6.6) |
| `actions` | The engine's own actions, apart from its layouts' (§5) |
| `settings.format`, `.schema`, `.defaults` | The settings format, a JSON Schema for the `engine` part of the file with titles in en, ko and ja, and the `engine` part of a new install (§8) |
| `limits.profiles` | How many henkan profiles it takes: at least 32 (§8) |

Schema: [schemas/describe.schema.json](schemas/describe.schema.json).

### 3.3 `P_session_new`

```json
{"harness": {"id": "com.teammirae.bada.mac", "version": "0.1.0", "features": ["segments", "pick", "around", "rtl"]},
 "layouts": [{"format": "bada-layout/3", "id": "…"}],
 "settings": {"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": []}},
 "dictionary": {"format": "bada-dictionary", "version": 1}}
```

| Field | Meaning |
|---|---|
| `harness.id`, `.version` | Required. The harness's id (§12) and its own version |
| `harness.features` | What the harness can do: `segments` (it draws them), `pick` (it sends `pick`), `around` (it answers a request for the text around the caret), `rtl` (it draws right to left). **An engine doesn't ask a harness for what it lacks** |
| `layouts`, `settings`, `dictionary` | Optional. Taken as the events of the same names would take them (§5), in that order: custom layouts, then the `engine` part of the settings file, then the user dictionary |

A problem in `layouts`, `settings` or `dictionary` doesn't fail the session: what is wrong isn't
loaded, as §6.6 says, and the session starts with the rest (with the engine's defaults, if the
settings were refused). The harness reads what was wrong by sending the same event with nothing to change (`{"type": "settings"}`,
`{"type": "layout"}`), which answers the problems of the last change. That is the last layout's, of
several: a harness that wants each layout's problems sends its layouts one `layout` event at a time
instead.

### 3.4 Memory and threads

As v1 (§4), with the session and borrowed buffers added:

- **A reply** belongs to the engine and stays valid **until the next `P_send` on the same engine**,
  or on the session when the engine is NULL. The harness copies what it keeps.
- **Arguments** are valid for the call only; the engine copies what it keeps. The one exception is a
  buffer passed with `P_BORROWED`: it **must** outlive the Data, and the engine may keep pointers
  into it.
- **Data** is immutable once built: sessions and engines on it may run on different threads at once.
- **An engine** is used by one thread at a time, and may move between threads. **A session** locks
  itself: session events may come from any thread while its engines are in use.
- **Nothing unwinds across the boundary.** An engine catches its own panics: the reply is
  `ok: false` with `error.code: "panic"`, and the engine has reset itself (§6.2). Where a panic
  can't be caught (WebAssembly, where it is a trap), the harness treats the trap as `panic` and
  makes the session and its engines anew.
- Text is UTF-8. Positions (caret, segment lengths) are counted in Unicode scalar values, never bytes.

## 4. Key codes

As v1 (§5): a key is named by its **W3C `KeyboardEvent.code`**, its physical position, and the
keyboard is **JIS** (日本語配列).

| Group | Codes |
|---|---|
| Number row | `Digit1`…`Digit0`, `Minus`, `Equal` (^), `IntlYen` (¥) |
| Letter rows | `KeyA`…`KeyZ`, `BracketLeft` (@), `BracketRight` ([), `Semicolon`, `Quote` (:), `Backslash` (]), `Comma`, `Period`, `Slash`, `IntlRo` (ろ, \\ _) |
| Control | `Space`, `Enter`, `Backspace`, `Tab`, `Escape`, `ArrowUp`, `ArrowDown`, `ArrowLeft`, `ArrowRight`, `Delete`, `Home`, `End` |
| F keys | `F1`…`F12` (bada takes `F6`–`F10` in henkan, per profile: §8) |
| Language keys | `Backquote` (半角/全角), `NonConvert` (無変換), `Convert` (変換), `KanaMode` (カタカナ/ひらがな; v1 wrote `Lang3`), `Lang2` (英数), `Lang1` (かな) |
| The lock | `CapsLock` |

- The first 48 (the number row through `IntlRo`) are the **typing keys**: the keys a layout types on.
- **Language keys and `CapsLock` never reach the engine.** The harness reads them (§9) and feeds the
  engine the result: a `select`, an `action`, or nothing.
- An engine **must** accept any string as a key's code; a code it doesn't know is a key it doesn't
  handle: it commits what's staged if that's its rule, with `consumed: false` (as v1).
- The keypad's Enter is sent as `Enter`.

## 5. Events

An event is a JSON object with a `type`. Schema: [schemas/event.schema.json](schemas/event.schema.json).

| `type` | Needs an engine | Fields | In v1 |
|---|---|---|---|
| `key` | yes | `key` (§4); `shift`, `ctrl`, `alt`, `meta` (missing: false); optional `repeat` | `P_key` |
| `action` | yes | `name` | `P_key` with `Lang1`, ⇧`Lang1`, ⌃`Lang1`; the 半角/全角 key |
| `select` | yes | `mode`, `profile` | `P_set_mode`, `P_set_profile` |
| `pick` | yes | `row` | none |
| `text` | yes | `selection`; `before`, `after` | `P_host_selection` |
| `flush` | yes | `cause`: `blur`, `click`, `edit`, `harness` | `P_flush` |
| `reset` | yes | | none |
| `state` | yes | | `P_state` |
| `settings` | no | `set` or `patch`; `dry` | `P_set_settings` |
| `layout` | no | `value`, or `id` with `value: null`; `dry` | none |
| `dictionary` | no | `value` | `P_dictionary_load` |

An engine ignores fields it doesn't know in an event whose type it knows. An unknown `type` answers
`ok: false` with `unknown-event`, and changes nothing.

- **`key`**: one press the harness wants read. The harness sends every key the key table doesn't
  take (§9), and never a language key or Caps Lock. It doesn't send a modifier or a lock pressed
  by itself (Shift, Control, Alt, Meta, Num Lock) either: that is no key press, and an engine that
  gets one changes nothing and passes it on. `repeat: true` marks a repeat of a held key.
  A key pressed with ⌃ or ⌘ is a shortcut: the engine commits what's staged and passes it on as the
  physical key, with no `types`, whatever the layout puts there (§7.2).
- **`action`**: a request by name, in place of v1's keys in disguise. The engine's own are in
  `P_describe.actions`; a layout's are `<layout id>.<action id>` (`ja-kana.katakana`, §7.5.8). A name
  the engine doesn't have answers `unknown-action`: it never does something else instead.

  | Name | Does |
  |---|---|
  | `menu.next`, `menu.prev` | Moves to the next or the previous row of the menu |
  | `fix` | **Fixes what shows**: the keys staged so far keep the text that shows, the top entry included, and stay staged; nothing is committed, and what was fixed no longer changes as more keys are typed. With nothing staged, nothing (as v1.3, §7.4) |
  | `width.full`, `width.half` | Makes the staged text full-width or half-width, and it stays staged in that width; with nothing staged, the selection (the reply asks for it: `ask.selection` is `"full"` or `"half"`) |

- **`select`**: the mode and the henkan profile of this text field. If either differs from what the
  engine has, it commits what's staged and switches; if neither does, nothing happens. `profile` is
  a profile id of the settings (§8): required in henkan; in muhenkan it is kept and echoed, and may
  be null. An id the settings don't have answers `unknown-profile`; henkan with no profile at all
  answers `no-profile`. Before its first `select` an engine is in muhenkan with no profile, and its
  replies have `mode: null`.
- **`pick`**: moves to row `row` of the last reply's menu, as `menu.next` would have got there.
  Nothing is committed: a harness that commits on a click sends `flush` after it. A row the menu
  doesn't have answers `bad-row`.
- **`text`**: the harness's answer to a reply's `ask`, once (§6.3): `selection`, the selected text
  (`""` if nothing is selected or something is marked); `before` and `after`, the text around the caret, no longer than asked (with text marked or
  selected: around that text). A `text` event nobody asked for changes nothing.
- **`flush`**: commits what's staged, as when the field loses focus; afterwards nothing is staged.
  `cause` says why: `blur`, `click` (a click in the text), `edit` (the app changed the text) or
  `harness` (the default, and what a harness sends when its platform doesn't say which).
- **`reset`**: drops what's staged. Nothing is committed.
- **`state`**: changes nothing; the reply is the last one again, with `commit` empty.
- **`settings`**, **`layout`**, **`dictionary`**: the session's events (§6.6, §7.6, §8, §11).

**No focus, app or field information** crosses. What a password field or a terminal gets is the
harness's choice of what to send.

## 6. Replies

Every event answers with the same object: what to do now, and everything that shows afterwards.
Schema: [schemas/reply.schema.json](schemas/reply.schema.json).

```json
{"ok": true,
 "commit": "", "replace": "marked", "consumed": true, "types": null,
 "preedit": {"text": "あなたinter", "caret": 8,
             "segments": [{"len": 3, "kind": "fixed"}, {"len": 5, "kind": "live"}]},
 "menu": {"rows": [{"text": "inter", "entry": 0, "lang": "en", "layout": "en-qwerty"},
                   {"text": "ㅑㅜㅅㄷㄱ", "entry": 1, "lang": "ko", "layout": "ko-2-set"}],
          "selected": 0, "strip": false},
 "mode": "henkan", "profile": "work", "staged": "あなたinter",
 "ask": null, "dir": "ltr"}
```

### 6.1 Fields

A missing field is its default. bada writes all of them. Engines may add fields; harnesses ignore
fields they don't know.

| Field | Default | Meaning | In v1 |
|---|---|---|---|
| `ok` | required | false: **nothing changed**, and `error` says why (§6.2) | `{}` |
| `error`: `{code, message, path}` | | Only with `ok: false` | none |
| `commit` | `""` | Text to insert now | the same |
| `replace` | `"marked"` | What `commit` replaces. `"marked"`: the marked text, or the selection when nothing is marked. `"selection"`: the selection, even when text is marked | `replace_selection` |
| `consumed` | false | The engine took the key. false: **the app gets the key too**, after the commit | the same |
| `types` | null | With `consumed: false` on a typing key or Space: the text the key should type, by the layout (§6.4). null: the engine has no say | The harnesses' own JIS legend tables |
| `preedit.text` | `""` | The marked text after the commit. `""`: nothing is marked | `preedit` |
| `preedit.caret` | the end | The caret's place in `text` | none |
| `preedit.segments` | `[]` | `text` cut into parts, in order: `len` and `kind`, one of `live` (still being read), `fixed` (`fix`, or a picked entry typed past: it no longer changes) and `picked` (the row the user moved to). A harness without `segments` draws `text` | none |
| `menu.rows[]` | `[]` | The menu's rows, top to bottom: `text`; `entry`, the signed index (0 the top reading, 1 and up the others, −1 and down a layout's forms, §7.5.12); optional `note` (romaji, a reading, a name), `lang` (BCP 47: the harness picks the font) and `layout` (whose reading it is) | `menu`, `notes`, `japanese`, `zero` |
| `menu.selected` | null | The highlighted row, or null for none | `selected`, past the end for none |
| `menu.strip` | false | The menu is one row of single characters | `horizontal` |
| `mode`, `profile` | null | As the harness last selected them: echoed, never changed by the engine | `P_state` |
| `staged` | `""` | **What `flush` would commit now**, character for character. It may differ from `preedit.text`, which is only what is drawn | `P_state` |
| `ask` | null | The engine needs text from the harness to finish: `{"selection": "hanja"}` (what for) or `{"around": {"before": 20, "after": 0}}` (how many characters). Answered with one `text` event | `need_selection` |
| `dir` | `"ltr"` | The writing direction of this reply's text (§6.5) | `direction` |

With these fields the last reply **is** the engine's state: v1's second call per key is gone, and
`state` only reads it again.

### 6.2 Errors

`ok: false` means nothing changed: the harness keeps what the last reply showed. `error.code`:

| Code | When |
|---|---|
| `bad-json` | The event isn't a JSON object |
| `bad-event` | A field is missing or has the wrong type; `path` names it |
| `unknown-event` | The engine doesn't take this `type` |
| `needs-engine` | The event is about a text field, and the engine was NULL |
| `unknown-action` | No action by that name; or its layout isn't in the profile in use |
| `unknown-profile`, `unknown-layout` | The id isn't in the settings, or among the layouts |
| `no-profile` | `select` asked for henkan, and the settings have no profile; or the engine's profile was removed and it has had no `select` since |
| `bad-row` | `pick` named a row the menu doesn't have |
| `invalid` | A `settings`, `layout` or `dictionary` event had an error-level problem (§6.6) |
| `panic` | The engine caught a panic. **It has reset itself**: nothing is staged. The harness deletes the marked text and closes the menu |

A harness that gets a code it doesn't know treats it as `bad-event`. **A refused `key` event is
swallowed**: nothing changed, and the app doesn't get the key either.

### 6.3 Applying a reply

The text field is a list of characters, a **selection** (a caret when empty) and an optional
**marked range**. Every reply is applied the same way, whatever the event (as v1 §7.1, with the new
names):

```
apply(r):
  if not r.ok:
      if r.error.code == "panic": delete the marked text; close the menu
      return
  if r.commit != "":
      range = the selection if r.replace == "selection" or nothing is marked, else the marked range
      replace range with r.commit; caret after it; nothing marked
  if r.preedit.text != "":
      range = the marked range if any, else the selection
      replace range with r.preedit.text; mark it; caret at r.preedit.caret in it
  else if something is marked:
      delete the marked text
  show r.menu; draw the preedit and the menu in r.dir
  if r.ask and r is not already the answer to one:
      apply(P_send(a text event with what was asked))        # once, never deeper

press(event):
  if the key table takes the press (§9): done
  r = P_send(the key event); apply(r)
  if r.ok and not r.consumed: the app gets the key (§6.4)
```

So **commit, then act** (Space, Enter, Esc with a preedit) is one reply with `commit` set and
`consumed: false`, as v1. An engine writes `replace: "selection"` only when it has nothing staged,
so nothing is marked then. A click in the text, or an edit by the app, commits what's staged: the
harness sends `flush` and applies it before the field changes (as v1 §7.6).

### 6.4 Keys passed on

A key the engine didn't consume goes to the app, which does what it does (as v1 §7.3). For a typing
key, `types` says what it should type:

- **`types` is a string**: the key types that text, whatever keyboard layout is selected in the OS.
  The harness lets the OS type it when the OS's layout gives the same text, and otherwise inserts
  the text itself and keeps the key from the app. This replaces the JIS legend table each v1 host
  carried (ろ types `\`, ⇧ `_`).
- **`types` is null**: the engine has no say: a control key, an F key, a shortcut, or no muhenkan
  layout chosen (§8). The app gets the key as the OS has it.
- A typing key whose slot is empty in the layouts in use (⇧0 in the JIS table, §7.2) types nothing:
  the engine consumes it.
- Caps Lock is the harness's (§9.4): with its lock on, the harness types the letters of `types` in
  capitals. The engine gets the same events either way.

### 6.5 Direction

As v1 (§6.1). Text is always in **logical order**, and positions are counted in it. `dir` changes how
the harness **draws**, never what it inserts: the base direction of the preedit and of the menu's
rows, and the side the menu is aligned to. An engine writes `"rtl"` only when it has the `rtl`
feature and the harness has it too; a reader takes a missing field, or any value but `"rtl"`, as
`"ltr"`.

### 6.6 Replies to the session's events

`settings`, `layout` and `dictionary` have no text field, so their replies have none of §6.1's
fields but `ok` and `error`. They have their own:

| Field | In the reply to | Meaning |
|---|---|---|
| `settings` | `settings` | The `engine` part in effect |
| `layouts` | `layout` | The session's layouts, stock and custom, in the shape of `P_describe.layouts`, with `stock` and `modified` as they are now |
| `dictionary` | `dictionary` | The summary of what is loaded: `{"positive": n, "negative": n}` |
| `problems` | all three | `[{path, code, message, level}]`: what was wrong, and where: `path` is into what the event carried (for `settings`, the `engine` part: `henkan.profiles[1].layouts[0]`) |

- A problem's `level` is `error` or `warning`. **An error-level problem changes nothing**: the reply
  is `ok: false` with `invalid`, and what was in effect stays in effect. A warning is taken, and
  noted.
- **`dry: true`** only checks: the reply is what the event would have answered, and nothing changes.
- **An event with nothing to change** (`{"type": "settings"}`, `{"type": "layout"}`,
  `{"type": "dictionary"}`) re-reads: what is in effect, and the problems of the last change.

Problem codes: `bad-format` (not this format's name), `bad-value` (the wrong type or range),
`unknown-field`, `unknown-layout`, `unknown-profile`, `unknown-action`, `unknown-option`,
`unknown-data` (a buffer the engine wasn't given: a warning, and its feature is off),
`duplicate-profile`, `too-many-profiles`, `not-muhenkan` (the muhenkan layout isn't one that types
directly); and for a layout, §7.6's.

### 6.7 The published state

A harness publishes the engine's state for scripts and for the settings page, in
`bada-compatible-state/2`: the last reply's display fields, and its own under `harness`.

```json
{"format": "bada-compatible-state/2",
 "mode": "henkan", "profile": "work", "staged": "한국어",
 "preedit": {"text": "한국어", "caret": 3, "segments": [{"len": 3, "kind": "live"}]},
 "menu": {"rows": [{"text": "한국어", "entry": 0}, {"text": "韓國語", "entry": 1}], "selected": 0, "strip": false},
 "ask": null, "dir": "ltr",
 "harness": {"id": "com.teammirae.bada.mac", "version": "0.1.0", "app": "com.apple.TextEdit",
             "secure": false, "active": true, "caps_lock": {"on": false, "setting": "respect", "rule": "allcaps"},
             "keys": {"available": ["Lang1", "Lang2", "CapsLock"], "chords": false, "missing": ["Accessibility"]},
             "problems": []}}
```

Under v1's rules (§12.1), which stand:

- **Read-only from outside.** Reading the state never changes the input method, and nothing a
  reader does is fed back to the engine.
- **Nothing typed is published in a password field**: `staged`, `preedit` and `menu` are empty, and
  `harness.secure` is true.
- The harness publishes after every reply to a text field's event, and when mode, profile, focus or
  Caps Lock change; only when the JSON changed.
- `harness.keys` says what the harness can detect now (§9.3), and `harness.problems` what was wrong
  with the settings, layouts and dictionary it last read, for the settings page to show: the
  engine's problems (§6.6) and the harness's own for `keys`, each with its file and with `path` into
  that file (`{file, path, code, message, level}`; `file` is `settings.json`, `layouts/<name>.json`
  or `dictionary.json`).
- The channel is the harness's, one the platform's scripting tools can read without a plugin; bada's
  are named in §12.

## 7. Layouts

### 7.1 Names

A layout's id is a slug: **`<group>-<name>`**, lowercase letters, digits and hyphens. The group is
the language (`en`, `ko`, `ja`: ISO 639-1) or `uni` for a layout that belongs to no language.
**There are no integer ids**: nothing in the settings, the ABI or the documents uses one.

### 7.2 The JIS table

Every layout types the JIS legend on every typing key it doesn't define itself, on every platform.
**The keyboard layout selected in the OS is never consulted.** The table is the engine's, once, so a
layout file can override it and no harness carries one; a harness gets the character with the key
(`types`, §6.4).

The letter keys type their letters, capitals with ⇧. The other 22:

| Key | Plain | ⇧ | | Key | Plain | ⇧ |
|---|---|---|---|---|---|---|
| `Digit1` | 1 | ! | | `IntlYen` | ¥ | \| |
| `Digit2` | 2 | " | | `BracketLeft` | @ | ` |
| `Digit3` | 3 | # | | `BracketRight` | [ | { |
| `Digit4` | 4 | $ | | `Semicolon` | ; | + |
| `Digit5` | 5 | % | | `Quote` | : | * |
| `Digit6` | 6 | & | | `Backslash` | ] | } |
| `Digit7` | 7 | ' | | `Comma` | , | < |
| `Digit8` | 8 | ( | | `Period` | . | > |
| `Digit9` | 9 | ) | | `Slash` | / | ? |
| `Digit0` | 0 | (nothing) | | `IntlRo` (ろ) | \ | _ |
| `Minus` | - | = | | `Equal` | ^ | ~ |

All 32 ASCII symbols are there, and ⇧0 is the one empty slot: it types nothing (§6.4).

**Shortcuts follow the QWERTY positions.** A key pressed with ⌘ or ⌃ is passed on as the physical
key, with no `types`, whatever the layout puts there: ⌘C is the key printed C under Dvorak and
Colemak too.

### 7.3 The stock layouts

| Id | Shown as | Kind | Reads (§7.5) |
|---|---|---|---|
| `en-qwerty` | English - QWERTY | types directly | `direct` |
| `en-qwerty-us` | English - QWERTY (US) | types directly | `direct` |
| `en-dvorak` | English - Dvorak | types directly | `direct` |
| `en-colemak` | English - Colemak | types directly | `direct` |
| `ko-2-set` | Korean - 2-set | composes | `hangul` |
| `ko-bada-roman` | Korean - bada roman | composes, ranked | `table` |
| `ja-romaji` | Japanese - Romaji | composes, converts | `table` |
| `ja-kana` | Japanese - Kana | composes, converts | `kana` |
| `uni-unicode` | Unicode - All Unicode | search | `search` |
| `uni-symbols` | Unicode - Symbols | search | `search` |
| `uni-emojis` | Unicode - Emojis | search | `search` |

Their files are in [engine/layouts/](../engine/layouts/). **There is no default layout**: a new
install has no muhenkan layout and no henkan profile (§8.3).

### 7.4 The stock layouts on a JIS keyboard

Each layout's established keys go on the 48 typing keys, and the rest is the JIS table. Where that
left a character with no key, it takes a free slot. The boards are drawn in
[layouts.html](layouts.html).

| Layout | Its own keys | Had no JIS key, and where it goes | Can't type |
|---|---|---|---|
| `en-qwerty` | none | | nothing |
| `en-qwerty-us` | A US keyboard's symbols, key for key by position: ⇧2 `@`, ⇧6 `^`, ⇧7 `&`, ⇧8 `*`, ⇧9 `(`, ⇧0 `)`, `- _`, `= +` on the ^ key, `[ {` on @, `] }` on [, `\ \|` on ], `; :`, `' "` on : | `` ` `` and `~` on ろ (plain, ⇧) | nothing. ¥ stays `¥`, with `\|` on ⇧¥ as well as on ⇧] |
| `en-dvorak` | Dvorak's letters and its own symbol places: `' "` on Q, `, <` on W, `. >` on E, `; :` on Z, `[ {` on -, `] }` on ^, `/ ?` on @, `= +` on [, `- _` on : | `@` and `` ` `` on the ] key (plain, ⇧); `~` on ⇧0; `^` on ⇧2; `*` on ⇧7 | nothing |
| `en-colemak` | Colemak's letters and its own `; :` on P | `+` on ⇧0 | nothing |
| `ko-2-set` | Jamo on the letter keys; ¥ is ₩ | | nothing |
| `ko-bada-roman` | The QWERTY letters, read as romanized Korean (`q` is ㅊ, `x` an initial ㅇ); `;` separates syllables and ⇧; types `;`; ¥ is ₩; ⇧0 is `+`; ⇧[ and ⇧] are 「 and 」; ⌥; is `·`; ⌥⇧ + a number or symbol key types its shifted JIS character | | `{` and `}` on plain and ⇧: they are on ⌥⇧[ and ⌥⇧] |
| `ja-romaji` | `-` ー, `,` 、, `.` 。, `/` ・, `[` 「, `]` 」; digits and other symbols full-width; ⇧7 is the apostrophe of `n'` | | Half-width characters: they are rows of the menu |
| `ja-kana` | Kana on all 48 keys; ⇧ for small kana, を and 、。・「」; ⌥⇧ + a number or symbol key for its shifted JIS character, full-width | | The digits 1–9, the unshifted symbols, and anything half-width, as JIS kana always has |
| `uni-*` | none: letters, digits and Space spell the search | | |

- **The Korean layouts type ₩ where ¥ is**; ⇧¥ stays `|`.
- **`en-dvorak`**: ⇧2 and ⇧7 no longer type the `"` and `'` printed on them (both are on Q), and the
  ] key types `@` and `` ` ``. ⇧ろ still types `_`, which is also on ⇧:.
- **`en-qwerty-us`**: the keys no longer match the JIS legends printed on them. 半角/全角 is not a
  typing key and keeps its default (§9.5), which is why `` ` ~ `` are on ろ.
- **`en-colemak`**: `:` is on two keys, ⇧P and its own. The JIS `;` key types o.
- **⌥⇧0** types `0` in `ko-bada-roman` and `０` in `ja-kana`, as in bada ime 0.3.1, though ⇧0 itself
  is empty in the JIS table.
- **`ja-kana`**: the harness sends `alt` and `shift` for the ⌥⇧ layer on every platform (Alt+Shift
  off the Mac).

### 7.5 A layout file: `bada-layout/3`

**Everything that makes a layout what it is lives in its JSON file, and the user can change any of
it, the stock layouts included.** The engine has no code for any one layout: it has a few general
ways of reading keys, and each is driven entirely by the file. What stays outside the file is bulk
data (a kanji dictionary, word counts, emoji and Unicode lists: named buffers, §11, which a file
refers to by name) and the general machinery, the same for every layout that asks for it: Hangul
composition, kanji conversion, width and script conversion, the search. A new *algorithm* is still
code.

Schema: [schemas/layout.schema.json](schemas/layout.schema.json). It describes every field, so a
layout editor can draw its fields from it. A field the format doesn't have is
a problem, not something to ignore. A `bada-layout/2` file is refused (`bad-format`): `/3` has the
`forms` part (§7.5.12) where `/2` listed script forms in `convert`.

```json
{"format": "bada-layout/3", "id": "ko-2-set", "name": {"en": "Korean - 2-set", "ko": "한국어 - 두벌식"},
 "lang": "ko", "kind": "henkan", "read": "hangul",
 "keys": {"KeyR": {"base": {"t": "ㄱ", "r": "c"}, "shift": {"t": "ㄲ", "r": "c"}},
          "KeyK": {"base": {"t": "ㅏ", "r": "v"}, "shift": {"t": "ㅏ", "r": "v"}},
          "IntlYen": {"base": {"t": "₩", "r": "x"}}},
 "hangul": {"vowels": {"ㅗㅏ": "ㅘ", "ㅜㅓ": "ㅝ"}, "finals": {"ㄱㅅ": "ㄳ", "ㄹㄱ": "ㄺ"},
            "double_tense": false, "final_moves_on": true},
 "actions": [{"id": "hanja", "label": {"en": "Hanja"}, "do": "lookup", "data": "hanja"}],
 "words": ["ko-words"], "rank": {"known": 100, "by_count": true, "whole": 80, "loose": 40}}
```

| Part | What it says |
|---|---|
| `format`, `id`, `name` | `"bada-layout/3"`; the slug (§7.1); what lists show, by language (`en` required) |
| `lang` | BCP 47: the language of its text, which a reply's rows carry. `und` for a layout of no language |
| `kind` | `muhenkan` (it types directly: `read` is `direct`) or `henkan` (it composes: any other `read`). Only a `muhenkan` layout can be the muhenkan layout; either kind can be a candidate of a henkan profile (§8) |
| `read` | Which general way of reading the keys: `direct`, `hangul`, `table`, `kana`, `search` |
| `keys` | Each key's layers and each slot's role (§7.5.1) |
| `direct`, `hangul`, `table`, `kana`, `search` | That way's own tables: exactly the part named by `read` (§7.5.2–§7.5.6) |
| `text_ends_word` | A text slot ends the word instead of being staged (§7.5.4) |
| `convert` | What turns the reading into more rows, and the data it uses (§7.5.7) |
| `actions` | The layout's named actions (§7.5.8) |
| `forms` | Other ways of writing what was typed, as the menu's negative entries (§7.5.12) |
| `particles` | Its particles that may follow a word typed on another layout (§7.5.9) |
| `words`, `rank` | The word lists that say which readings are known words, and the scores of its readings (§7.5.10) |
| `options` | The options it has in the settings (§7.5.11) |
| `note` | Where the layout comes from. The engine doesn't read it |

#### 7.5.1 Keys, layers and roles

`keys` maps a key code (one of the 48 typing keys, or `Space`) to its **layers**: `base` (no
modifier), `shift` (⇧), `alt` (⌥) and `alt_shift` (⌥⇧). A layer holds one **slot**:
`{"t": "ㄱ", "r": "c"}`, what the key types there (`t`) and its role (`r`), or `null`.

- **A `base` or `shift` slot left out is the JIS table's** (§7.2): a letter with the role `l` where
  the way of reading has letters, and otherwise text (`x`). `null` is an empty slot: the key types
  nothing there.
- **`alt` and `alt_shift` are the layout's only where it lists them.** Elsewhere ⌥ + a key isn't
  the layout's: what's staged is committed and the key goes to the app.
- **`Space` is the layout's only where it lists it.**
- ⌃ and ⌘ are never a layer (§7.2).

| Role | In | Meaning |
|---|---|---|
| `x` | every way | Text, typed as it is |
| `l` | `direct`, `table`, `search` | A letter: of a word, of a spelling, of the search |
| `c`, `i`, `f` | `hangul` | A consonant: both an initial and a final; an initial only; a final only |
| `v` | `hangul` | A vowel |
| `b` | `table` | A break between syllables |
| `k`, `m` | `kana` | A kana; a mark that attaches to the kana before it |

**Which keys are staged, in henkan.** A press is staged when a layout of the profile has a slot for
it on that layer, and a layout reads the staged keys only if it has a slot for every one of them.
`Space` is staged only when every layout of the profile lists it. A press no layout has a slot for
commits what shows: an empty slot of a typing key then types nothing, and any other key goes to the
app.

#### 7.5.2 `direct`

Every key types its slot. In muhenkan the reply is `consumed: false` with `types` (§6.4); as a
candidate in a henkan profile, the layout's reading is the text of the staged slots.

- **`variants`**: a character → its variants. In muhenkan, ⌥ + a key whose slot's character has
  variants (⌥⇧ for its ⇧ slot), and no `alt` slot of its own, is consumed: the character shows as the
  preedit with its variants as a strip (`menu.strip`), none selected (§14).

#### 7.5.3 `hangul`

Jamo compose into syllables. The reading is precomposed syllables (U+AC00–U+D7A3), and
compatibility jamo for what makes no syllable.

| Table | Meaning |
|---|---|
| `vowels` | Two vowels that join into one: `"ㅗㅏ": "ㅘ"` |
| `finals` | Two finals that cluster into one: `"ㄱㅅ": "ㄳ"` |
| `double_tense` | The same initial twice before a vowel, or the same final twice, is its tense form (ㄱ ㄱ → ㄲ; as finals ㄲ and ㅆ only) |
| `final_moves_on` | A vowel after a final takes the last final as the initial of a new syllable, when the slot that typed it has an initial form (`c`). An `f` final never moves |
| `implied_ieung` | A vowel with no initial to join starts a syllable on ㅇ. Default false: it shows as a jamo |

- A consonant with a final form (`c`, `f`) after a syllable that has a vowel joins it as its final:
  always the first, and a second when `finals` has the pair. Otherwise a consonant with an initial
  form (`c`, `i`) starts a syllable; an `f` with nothing to join shows as a jamo.
- A vowel joins an initial that waits for one; after a vowel with no final it joins when `vowels`
  has the pair.
- A text slot is typed as it is, between syllables, and stays staged with them.

#### 7.5.4 `table`

Letters spell text. The slots with the role `l` feed a run of letters, matched without regard to
case; a text slot ends the run and is typed as it is. The `table` part has one of two forms.

**`spellings`**, for text that is spelled a piece at a time (romaji):

```json
"table": {"spellings": {"ka": "か", "n": "ん", "nn": "ん", "n'": "ん"},
          "rules": [{"in": "nn", "out": "ん", "before": "aiueo", "keep": 1},
                    {"in": "kk", "out": "っ", "keep": 1}]}
```

- A **rule** is a spelling that looks ahead or keeps letters: `before` makes it apply only when the
  next letter is one of those (that letter isn't used up), and `keep` leaves its last letters
  pending (`kk` types っ and leaves one `k`).
- The pending letters are read whenever a letter is added, longest first. While they are the start
  of a longer spelling, or of a rule that still needs its next letter, they wait. Otherwise the
  longest spelling or rule that begins them is typed, a rule with `before` ahead of a spelling of
  the same letters; a letter that begins none is typed as it is. A text slot, and the end of the
  staged keys, read what is pending without waiting.

**`syllable`**, for Hangul spelled an onset, a vowel and a coda at a time (bada roman):

| Table | Meaning |
|---|---|
| `onsets`, `vowels`, `codas` | A spelling → the jamo it reads, the default first: `"ch": ["ㅊ", "ㅈ"]`. The longest spelling that matches is tried first |
| `empty_onset` | The initial of a syllable that starts with a vowel: `["ㅇ"]` |
| `vowels_before_coda` | What a vowel spelling reads instead when a final follows: `"yu": ["ㅕ", "ㅠ"]` |
| `codas_before` | What a coda spelling also reads before a given letter: `l` before `t` is ㅀ too |
| `letter_after` | A text slot that is a letter when the letter before it is one of these: `"-": ["e"]` makes `e-` a spelling |
| `shortcuts` | Other spellings of whole syllables, from a named buffer; `option` names the option that turns them on |

- A run is read syllable by syllable. At each place: every onset that matches, or the empty onset
  where a vowel spelling starts; then every vowel that matches; then the consonant letters up to the
  next vowel letter (a letter that occurs in a vowel spelling), of which a coda takes a prefix, or
  none.
- **The default reading comes first.** A coda is preferred when it leaves nothing, or leaves exactly
  one onset spelling for the next syllable; among preferred codas the shortest comes first, and then
  the others, longest first. So `hanguk` is 한국 and `hang;uk` 항욱.
- **Every other reading follows**, for the ranking (§7.5.10): each jamo a spelling lists, each other
  split, each shortcut. Letters that make no syllable show as the first jamo of their onset, or as
  themselves.
- A `b` slot ends a syllable and types nothing.

**`text_ends_word`** (any way of reading; bada roman uses it): a text slot commits what shows and
then types its character, so it can't change the reading before it. It is staged as usual instead
when another layout of the profile, one that isn't `direct`, has a slot for that key that isn't
text; and beside a `direct` layout while that layout's reading is the one that shows (`U.S.` is
typed through as English).

#### 7.5.5 `kana`

One kana per key. A `k` slot types its kana. An `m` slot is a mark: `marks` says what it makes of the
kana before it (`"゛": {"か": "が"}`), and where it makes nothing the mark stays as itself. A text slot
is typed as it is, and stays staged with the kana.

#### 7.5.6 `search`

The keys spell a search, and the menu lists what it finds: the search as typed first, then the
results, each with a note. The search is the text of the staged slots, letters in lower case.

| Field | Meaning |
|---|---|
| `sources[]` | What is searched: `data`, a named buffer, and `match`, its columns that are searched. A result must have, for every word of the search, a word that begins with it |
| `sources[].tiers` | How its matches rank, best first: `exact` (the search is the whole first column), `whole` (a search of one word is a whole word of a column), `first` (every word begins a word of the first column), `any` |
| `sources[].order` | Within a tier: `data` (the buffer's order) or `shortest` (the shorter first column first) |
| `sources[].note` | The row's note: `name`, or `code` (`U+XXXX` and the name) |
| `sources[].single` | A column of characters: a search of one character lists the entries that have it there, and nothing else |
| `sources[].forms` | With `single`: a letter or digit that no entry lists offers its other forms: full-width and circled, and a digit's superscript, subscript and roman numeral |
| `sources[].tones` | The option that orders an emoji's skin tones; a tone left out isn't offered |
| `code` | A character by its code: up to `max_digits` hex digits, after one of `after`, or after one of `digit_first_after` when the first digit is 0–9 |
| `min_length` | The shortest search that looks names up |
| `limit` | Results listed, at most |

Results of several sources are ordered by tier, then by source.

#### 7.5.7 `convert`

A list of what turns the layout's reading into more rows, each naming one of the engine's general
conversions with its arguments.

| `with` | Does | Arguments |
|---|---|---|
| `kanji` | Kanji conversion of a kana reading | `data`: the dictionary; `rows`: conversions listed; `user_rows`: how far down the user's own words are looked for; `common`: a list of common words by reading, and its rows; `native`: the list of words written in kana; `notes`: `kana` rows carry their `romaji`, converted rows their `reading` |
| `width` | The reading with its letters, digits and symbols in the other width, as a row, where it differs; kana stay as they are | `to`: `half` or `full` |
| `dictionary` | A dictionary lookup of the letters typed, when every staged key is a letter | `data`; `rows`; `first: false`: never the top row; `below_top`: how far under the top reading its rows start; `case`: which of the dictionary's lists by how the word was typed (`lower`, `mixed`, `upper`: `word`, `name`, `name-then-word`, `name-else-word`). Each row's note is the word as typed |

#### 7.5.8 `actions`

The layout's named actions, for a profile's F keys (§8) and for `action` events
(`<layout id>.<action id>`). Each has an `id`, a `label` by language, and `do`, one of the engine's
general operations:

| `do` | Does | Argument |
|---|---|---|
| `script` | Shows the reading in a script, until the commit | `to`: `hiragana`, `katakana`, `halfwidth-katakana` |
| `keys` | Shows the staged keys as their JIS characters (§7.2) in a width, until the commit | `width`: `full`, `half` |
| `jamo` | Leaves one jamo of each syllable of the staged text, which stays staged; with nothing staged, of the selection | `keep`: `initials`, `vowels`, `finals` |
| `lookup` | Opens a list from a named buffer for the staged text; with nothing staged, for the selection | `data` |

An action acts for a layout among the candidates of the profile in use. **Staged text comes before
the selection**: an action that can take either asks for the selection (`ask`, §6.1) only when
nothing is staged.

#### 7.5.9 `particles`

With a `direct` layout in a profile, one staged run may be a word typed on it and then a particle
typed on another layout of the profile (`Claude에`, `GitHubで`). That other layout lists what may
follow: `words`, its particles and endings; `after`, what they may follow (`word`: a known word of
the `direct` layout; `name`: a capitalised word its lists don't have); and `stack`, when particles
stack: the ones that may come `first`, and `max`, how many in all.

#### 7.5.10 `words` and `rank`

`words` names the word lists that say which of the layout's readings are known words; the user's
dictionary (§11) counts with them. `rank` scores each kind of reading, before the profile's weights
and the comparison between layouts (§7.7):

| Field | The reading |
|---|---|
| `known`, `by_count` | Is a known word. With `by_count`, the natural logarithm of its count is added |
| `rare` | Is in a list's rare section |
| `letters`, `other` | `direct`: letters only, but no known word; anything else |
| `whole`, `loose` | Hangul in whole syllables, but no known word; with loose jamo left over |
| `unknown`, `broken` | Kana that is no known word; with a mark left over or Latin letters pending |
| `after_text` | `table`: has letters after a text slot that should have ended the word (§7.5.4) |
| `query`, `found` | `search`: the search as typed; what it finds |
| `readings` | `table`, `syllable` form: another reading that is a known word scores `known` less `cost` for each jamo it changes from the default (at most `max_cost`), never under `floor`; `rows` of them at most. Splitting the letters elsewhere changes no jamo |
| `model` | When no reading is a known word: the reading a jamo model (`data`) finds most likely, at `cost` per jamo changed, shows first when it beats the default by `margin` (both base-10 logarithms) |

#### 7.5.11 `options`

The options the layout has in the settings, under `options.<layout id>` of a profile or of the
defaults (§8): a name → its `type` (`boolean`, or `order`: some of the values `of`, in an order),
its `default`, and a `title` and optional `hint` by language. The engine reports them in
`P_describe.layouts[].options`, and the settings page draws them.

#### 7.5.12 `forms`

Other ways of writing what was typed, as **the menu's negative entries**: the first of the list is
entry −1, the second −2, and so on. A layout lists as many as it wants. Each is one of the engine's
general operations that needs **nothing but the staged keys and this file**: no data, no word list,
no ranking, nothing of the user's. So the same keys always give the same forms, at the same entries.

```json
"forms": [{"do": "script", "to": "hiragana"}, {"do": "script", "to": "katakana"}]
```

| `do` | The form | Argument |
|---|---|---|
| `script` | The layout's reading of the keys, in a script | `to`: `hiragana`, `katakana`, `halfwidth-katakana` |
| `keys` | The keys as their JIS characters (§7.2), in a width | `width`: `full`, `half` |
| `jamo` | One jamo of each syllable of the layout's reading | `keep`: `initials`, `vowels`, `finals` |

- **Forms show in a profile that has `script_forms` on** (§8.1), and they are one layout's:
  `script_forms_layout`, or the first layout of the profile that lists any. While that layout can't
  read the staged keys there are none.
- **Every form is a row, in its place, whatever it says**: two forms that come out the same are both
  listed, and so is one that says what entry 0 says. An entry never moves because of what was typed.
- A conversion (§7.5.7) that a form already writes isn't listed again from entry 0 up: with the
  stock Japanese layouts, the katakana. A word of the user's own (§11) still is.
- **Typing on doesn't fix a form**, as it does a row picked above entry 0: the reading goes on at
  entry 0. `fix` keeps a form that shows, like any entry.
- After a `fix`, the forms are of the keys typed since.

The stock `ja-romaji` and `ja-kana` list hiragana (−1) and katakana (−2).

### 7.6 Stock and custom layouts

- **The stock layouts are files like any other**, shipped with the engine. A layout with a stock id
  that the harness feeds **replaces** the stock one for the session; removing it brings the stock
  one back.
- **A custom layout is fed by a `layout` event**: `value`, a layout object, adds it or replaces the
  one with its id; `id` with `value: null` removes it. `P_session_new`'s `layouts` are the same
  thing at the start. The harness keeps the files (§10.1); the settings page's layout editor, the C
  ABI and the command line (§10.2) all come to this one event.
- **The engine checks each.** An error-level problem means the layout isn't loaded (a stock one
  falls back to the shipped file):

  | Code | Level | When |
  |---|---|---|
  | `bad-format`, `missing-id`, `bad-value`, `unknown-field` | error | Not a `bada-layout/3` file; no `id`; a field of the wrong shape; a field the format doesn't have |
  | `unknown-key` | error | A key code that isn't one of the 48 or `Space` |
  | `bad-role` | error | A role its way of reading doesn't have |
  | `contradiction` | error | A table that contradicts itself: `kind` and `read` that disagree, a part for another way of reading, a spelling that can never be typed, an option used but not declared |
  | `in-use` | warning | The settings in effect name the layout, and it is gone, or replaced by one they can't use there. The layout goes all the same; the settings stay in effect without it, and now have a problem of their own (§6.6) |
  | `unknown-data` | warning | It names a buffer the engine wasn't given: that feature is off |
  | `stock-modified` | warning | It replaces a stock layout (§7.7) |
  | `no-model` | warning | Its id has no key model (§7.7) |

- The reply to a `layout` event lists the session's layouts (§6.6): a stock layout that was replaced
  has `modified: true`, and a custom one `stock: false`.

### 7.7 Ranking between layouts

How a profile's layouts are compared is the engine's. bada's key models, which decide which layout a
run of keys was meant for, are trained on the stock layouts and stored under their ids, so they
can't follow an edit:

- **A stock layout that was changed** keeps its id, so its model is still used, and the model still
  believes the stock keys. In a profile with other layouts, the wrong layout's reading may show.
- **A layout with a new id** has no model. Among other layouts it is ranked by its `rank` scores
  alone.
- A layout used alone in a profile is unaffected either way.

A key that types the same text in every layout compared, in whatever width, says nothing about which
was meant: a digit beside Korean and English, or `3` beside `３`. The models don't see such a key
unless no other key is staged, so in `2022년` and `３月` the keys after the number decide.

So the two warnings of §7.6. **The settings page shows the warning before it saves a change to a
layout's JSON**, where the JSON is edited, saying which of the two applies and what it costs; a
changed stock layout carries a "modified" mark in every list from then on. The command line refuses
a change to a stock layout without `--modify-stock`, and says why (§10.2).

## 8. Henkan profiles

A henkan profile is **an entry in an ordered list in the engine's settings**. The user makes as many
as they want; the list's order is the display order.

```json
{"format": "bada-settings/2",
 "engine": {
   "muhenkan": {"layout": "en-qwerty"},
   "henkan": {
     "defaults": {"weights": {}, "script_forms": false,
                  "options": {"uni-emojis": {"skin_tones": ["none", "medium"]}}},
     "profiles": [
       {"id": "main",  "name": "한국어",  "layouts": ["ko-2-set"]},
       {"id": "work",  "name": "work",    "layouts": ["en-qwerty", "ko-2-set", "ja-romaji"], "weights": {"en-qwerty": 0.8}},
       {"id": "ja",    "name": "日本語",  "layouts": ["ja-kana"], "script_forms": true,
        "fkeys": {"intercept": true, "keys": {"F7": "ja-kana.katakana"}}},
       {"id": "sym",   "name": "symbols", "layouts": ["uni-symbols", "uni-emojis"]}
     ]
   }
 },
 "keys": {},
 "harness": {"com.teammirae.bada.mac": {}}}
```

The settings file has three parts: `engine` (this section; **the only part the harness sends to the
engine**), `keys` (the harness's key table, §9) and `harness` (one harness's own settings, by its
id). Schema: [schemas/settings.schema.json](schemas/settings.schema.json). The shape of `engine`
below is bada's; another engine publishes its own in `P_describe.settings.schema`, and keeps
`henkan.profiles[].id` and `.name`, which the harness reads.

### 8.1 The fields

| Field | Meaning |
|---|---|
| `muhenkan.layout` | The muhenkan layout: a layout that types directly, stock or custom. Unset, as on a new install, the engine passes every key on and the OS types it |
| `henkan.defaults` | What profiles share: `weights`, `script_forms`, `script_forms_layout`, `options`. A profile overrides it field by field |
| `henkan.profiles[].id` | `[a-z0-9-]{1,32}`, unique, chosen by the user, never the name of a key. The key table, the state and `select` refer to a profile by it |
| `.name` | What menus show |
| `.layouts` | The candidates, by layout id, stock or custom, in order: earlier wins a tie. The `en-*` layouts may be candidates |
| `.weights` | A multiplier on every reading a layout raises, by layout id: 0 to 10, 1 when missing |
| `.script_forms`, `.script_forms_layout` | A layout's forms as negative menu entries (§7.5.12), off by default; and the layout whose forms they are, or null for the first layout of the profile that lists any |
| `.options` | Per-layout options, by layout id, as each layout's file declares them (§7.5.11) |
| `.fkeys` | **This profile's own F6–F10**, with nothing shared between profiles: `intercept` (false: every F key goes to the app), and each key `swallow`, `pass` or `<layout id>.<action id>`. A profile without `fkeys` intercepts them and binds none. The harness feeds the engine the F keys as ordinary `key` events |

- **No fixed count, no reserved names.** None is allowed, and is how a new install starts (§8.3).
  The format has no upper limit; an engine **must** take at least 32 profiles, and **must** refuse
  more than it takes with an error (`too-many-profiles`), never truncate.
- **No links.** A profile the user doesn't want doesn't exist.
- **An unknown id is an error** (`unknown-profile`, `unknown-layout`), in `select` and in the
  settings alike: nothing falls back to another profile.
- **Where the user sees it.** The OS gets exactly two input sources, one for each mode, where it
  has an input source per mode (macOS); elsewhere one input method holds both modes.
  Profiles are inside henkan: the harness's input menu lists `henkan.profiles` by `name`, and
  replies echo `profile`, for an indicator. A harness that keeps the mode across processes stores
  the id string, never an index.

### 8.2 Changing the settings

- The harness sends the engine the `engine` part: a `settings` event with `set` replaces it, and
  with `patch` merges a JSON merge patch (RFC 7396) into it. The reply has the `settings` in effect
  and the `problems` (§6.6).
- **Invalid JSON, a wrong `format`, or an error-level problem changes nothing**: the last good
  settings stay in effect, and the problems are in the state for the settings page to show (§6.7).
  v1 reset every setting to its default there.
- An engine whose profile was changed commits what it has staged at its next event. An engine whose
  profile was removed does the same, and then answers `no-profile` until the harness sends it a
  `select`.

### 8.3 A new install

The first settings file has **no muhenkan layout and zero henkan profiles**.

- With no profile, a key that would enter henkan (かな, 変換, a toggle) doesn't: the harness opens
  the settings window on its **onboarding**. The engine is not asked.
- **Easy mode**: pick the muhenkan layout and one henkan layout (`en-qwerty` and `ja-romaji`, say).
  That writes `muhenkan.layout` and one profile with that one layout (its id `main`, its name the
  layout's), and the key that was pressed works from then on.
- **Skip**: the full settings, right away.
- The same from the command line, for an agent: `bada settings set engine.muhenkan.layout
  '"en-qwerty"'` and `bada profile add main --layouts ja-romaji`.

Nothing is carried over from bada ime: bada is a new input method beside it (§12), and never reads
the old one's settings.

## 9. Keys the harness decides

The engine never sees a language key or Caps Lock. The harness reads them, decides, and feeds the
engine the result: a `select`, an `action`, or nothing.

### 9.1 The key table

The `keys` part of the settings file. It is the harness's, in one format for every harness, with
keys as W3C codes so a file moves between platforms:

```json
"keys": {
  "bindings": [
    {"on": "CapsLock",    "do": ["mode.toggle"]},
    {"on": "Lang2",       "do": ["mode:muhenkan"]},
    {"on": "Lang1",       "do": ["profile:main"]},
    {"on": "shift+Lang1", "do": ["menu.prev"], "when": "henkan"},
    {"on": "ctrl+Lang1",  "do": ["fix"], "when": "henkan"},
    {"on": "Convert",     "do": ["profile:work", "menu.next"]},
    {"on": "KanaMode",    "do": ["profile:ja", "menu.next"]},
    {"on": "Lang1",       "do": ["menu.next", "profile:main"], "when": "henkan"},
    {"on": "KanaMode+Lang1", "do": ["profile:sym"]},
    {"on": "ctrl+alt+shift+meta+Space", "do": ["profile.next"]}
  ],
  "cycle": ["main", "work", "ja"],
  "henkan_enters": "last",
  "caps_lock": "respect",
  "caps_rule": "allcaps"
}
```

- **`on`**: one key, with or without modifiers, as recorded from a real press: the modifiers in the
  order `ctrl`, `alt`, `shift`, `meta`, then the key's code. Or a **pair** of language keys (§9.3).
  A binding matches a press of the same key with exactly the same modifiers.
- **`when`**: `muhenkan`, `henkan`, or `profile:<id>` (henkan, with that profile in use). The
  binding holds only then. **The most specific binding that holds wins** (a profile's, then a
  mode's, then one with no `when`); among equals, the later one in the list.
- **`do`**: a list, and **the first action that applies runs** (§9.2). If none applies the key is swallowed. A binding
  runs once for each press: the repeats of a held key that has a binding are swallowed, but for a key that was passed on
  (`pass`), whose repeats pass too.
- **`cycle`**: the profiles `profile.next` and `profile.prev` go through, in order. One it names
  that the settings don't have is skipped.
- **`henkan_enters`**: the profile that `mode:henkan` and the toggle enter: `"last"`, the one last
  used (the first of the list, before any was), or a profile's id.
- **`caps_lock`**, **`caps_rule`**: §9.4.

**A press the table has a binding for isn't fed to the engine.** A press it has none for, or none
that holds, is not the table's: an ordinary key goes to the engine as a `key` event, and a language
key or Caps Lock, which never go to the engine, go to the OS as they are.

The harness knows all it needs to decide: its own mode and profile, the profile list, and the last
reply.

### 9.2 Actions

| Action | The harness then | Doesn't apply when |
|---|---|---|
| `mode.toggle` | Goes to the other mode: `select` with muhenkan, or with henkan and `henkan_enters`' profile | never |
| `mode:muhenkan`, `mode:henkan` | Switches the OS's input source, and sends `select` | Already in that mode |
| `profile:<id>` | Enters henkan if needed, and sends `select` with the profile | That profile is in use, in henkan; or no profile has that id |
| `profile.first` | The same, with the first profile of the list | It is in use, in henkan |
| `profile.next`, `profile.prev` | The same, with the profile after or before the one last selected in `cycle`, wrapping round; the first or the last of `cycle` when that one isn't in it | `cycle` is empty; or it leads to the profile in use, in henkan |
| `profile.last` | The same, with the profile used before this one; in muhenkan, the one last used | There is none |
| `menu.next`, `menu.prev` | Sends `action` | The last reply's menu has one row or none |
| `fix` | Sends `action` | Nothing is staged |
| `width.full`, `width.half` | Sends `action` | never |
| `<layout id>.<action id>` | Sends `action` | Not in henkan; or the layout isn't a candidate of the profile in use |
| `pass` | Lets the app have the key | never |
| `none` | Swallows the key | never |

An action doesn't apply when it would change nothing, and then the next one of the list is tried.
So a key can be the toggle only while there is nothing to step through: `{"on": "shift+Tab",
"do": ["menu.prev", "mode.toggle"]}` is the entry before while the menu has two rows or more, and
the toggle otherwise. No preset binds ⇧Tab (§9.5): unbound, it is the engine's key.
**With no profile at all, any action that would enter henkan opens the onboarding instead** (§8.3).
Whenever a `select` changes the profile, the one before it is what `profile.last` goes back to.

### 9.3 The keys a binding can be on

**Language keys.** `mode.toggle`, like every action, can be bound to any language key the harness
detects:

| Key | Code | Windows | Linux | macOS |
|---|---|---|---|---|
| 半角/全角 | `Backquote` | yes | yes | yes: a PC JIS board's key arrives as the `` ` `` key. Apple's JIS boards don't have it |
| 無変換 | `NonConvert` | yes | yes | Read at the HID level: needs Input Monitoring |
| 変換 | `Convert` | yes | yes | The same: macOS makes no key event of it |
| カタカナ/ひらがな | `KanaMode` | yes | yes | The same: macOS turns it into かな |
| 英数 | `Lang2` | | | yes; everywhere once Accessibility is allowed |
| かな | `Lang1` | | | yes; everywhere once Accessibility is allowed |
| Caps Lock | `CapsLock` | yes | yes, but an input method can't set the lock, so its light follows the key | Needs Input Monitoring and Accessibility; without them it is macOS's |

A key bound here isn't fed to the engine: 半角/全角 bound to the toggle gives up full-width and
half-width on that key, which is its default (§9.5).

**Pairs of language keys.** Two language keys held together are a binding of their own:
`"on": "KanaMode+Lang1"` (かな with カタカナ), `"Convert+Lang1"`, any two different keys of the table
above but Caps Lock, written in either order, with no modifiers. A pair can be bound to any action,
wherever the harness detects both keys; no default binds one (§9.5).

- A pair is **both keys pressed, in either order, and both released, with no other key pressed in
  between**. Its binding runs when the second of them is released.
- If another key is pressed before both are released, it isn't a pair: nothing runs for either of
  them, and the other key is handled as usual.
- **A key that is part of a bound pair runs its own binding late**: not when it is pressed, but when
  it is released alone, or as soon as another key that makes no bound pair with it is pressed
  (before that key is handled). A key that is part of no bound pair runs when it is pressed, as always.
- **A key waits only for a pair binding that holds when the key is pressed** (its `when`): a pair
  bound for henkan makes no key wait in muhenkan.
- "Another key" is any key, a modifier included. The repeats of a held key are not presses.
- A key that waited and turns out to have no binding of its own goes to the OS then, where the
  platform lets the harness post a key; elsewhere it is dropped.

**Any other key, once the OS allows it.** With the permissions it needs, the harness takes any chord
as a binding: `ctrl+alt+shift+meta+Space`, an F key, a modifier pressed and released alone (its code, `AltRight`), which runs when it is released if no
other key was pressed in between.

- **macOS**: Accessibility (the key tap), and Input Monitoring for keys read at the HID level.
- **Windows, Linux**: the text service is handed the focused app's keys, modifiers included, so
  chords need no permission; a chord the desktop takes first never arrives.
- The harness publishes what it can detect now, in the state (§6.7): `harness.keys.available` (the
  language keys of the table above that it sees), `harness.keys.chords` (true or false) and
  `harness.keys.missing` (the permissions it lacks). The settings page and the command line read
  it: they offer chord bindings only when it is true, and list a binding the harness can't see as
  inactive instead of dropping it.

### 9.4 Caps Lock

Bound to anything, Caps Lock stops being Caps Lock: its binding runs and the lock doesn't change
(but `pass`, which gives the key back: it is the lock again).
⇧, ⌃, ⌥ or ⌘ with Caps Lock are other chords, with bindings of their own or none. Unbound, the key
is the lock, and two settings say what the lock does, as v1 (§7.7):

| `caps_lock` | The lock |
|---|---|
| `respect` (the default) | Caps Lock everywhere: henkan doesn't turn it off |
| `transient` | Caps Lock in muhenkan only. It is turned off when muhenkan is left; pressed in henkan it does nothing; if another program turns it on in henkan, the harness turns it off |

- The lock affects only what the **app** is given: with it on, the letters of `types` are typed in
  capitals (§6.4). The engine gets the same events, so henkan composition ignores it.
- **`caps_rule`** says what the lock types in muhenkan: `allcaps` (the default), or `upper_snake`,
  where the harness also flushes and types `_` for Space (with ⇧ or without) and a space for ⌥Space, without sending the key.

### 9.5 Defaults

Defaults come with the keyboard, so nothing in the standard is "home". A settings file with no
`keys.bindings` gets its keyboard's preset; one that has them, an empty list included, is used as
it is. A harness that can't tell which keyboard it has lets the user choose, and keeps the choice in its
own part of the settings as `keyboard`: `auto`, `jis-mac`, `jis-pc` or `none` (no language keys). The other fields
default to `"cycle": []`, `"henkan_enters": "last"`, `"caps_lock": "respect"`, `"caps_rule": "allcaps"`.

**JIS on a Mac**: v1's behaviour, key for key.

```json
[{"on": "Lang2",           "do": ["mode:muhenkan"]},
 {"on": "Lang1",           "do": ["profile.first"]},
 {"on": "Lang1",           "do": ["menu.next", "profile.first"], "when": "henkan"},
 {"on": "shift+Lang1",     "do": ["menu.prev"], "when": "henkan"},
 {"on": "ctrl+Lang1",      "do": ["fix"], "when": "henkan"},
 {"on": "Backquote",       "do": ["width.full"]},
 {"on": "shift+Backquote", "do": ["width.half"]}]
```

**JIS on a PC**: the same on 無変換 and 変換; カタカナ is unbound.

```json
[{"on": "NonConvert",      "do": ["mode:muhenkan"]},
 {"on": "Convert",         "do": ["profile.first"]},
 {"on": "Convert",         "do": ["menu.next", "profile.first"], "when": "henkan"},
 {"on": "shift+Convert",   "do": ["menu.prev"], "when": "henkan"},
 {"on": "ctrl+Convert",    "do": ["fix"], "when": "henkan"},
 {"on": "Backquote",       "do": ["width.full"]},
 {"on": "shift+Backquote", "do": ["width.half"]}]
```

**No language keys**: `[{"on": "CapsLock", "do": ["mode.toggle"]}]`.

- With no profile yet, かな and 変換 open the onboarding (§8.3).
- **かな on a Mac and 変換 on a PC are the next entry in every profile**, and with ⇧ the previous
  one, whenever the menu has two rows or more. With one row or none there is nothing to step
  through, and the key goes to its profile. A table that gives other profiles keys of their own
  keeps this: for henkan, `menu.next` first and then the profile, as in §9.1's.
- **半角/全角 makes full-width, and ⇧半角/全角 half-width, by default on every keyboard that has the
  key**: the staged text, or else the selection. They are the last two lines of the JIS presets, so
  the user sees them and can rebind the key (to the toggle, say).
- **F keys are not in this table**: they are each profile's own, in the engine's settings (§8.1).

### 9.6 Each harness reads the table itself, and the cases

There is no shared code for this section: every harness, and the command line, carries out §9.1 to
§9.5 in its own language. What they share is the table's format and its rules, which are this
standard's, so one settings file means the same on every platform.

To keep the copies from drifting, the standard also carries **cases**:
[cases/key-table.json](cases/key-table.json) (`bada-key-cases/1`, schema
[schemas/key-cases.schema.json](schemas/key-cases.schema.json)). Each case is a key press, the
harness's state (mode, profile, the menu's rows, what is staged, Caps Lock), a table and a profile
list, with what must happen: the binding chosen, the action that ran, and the result (`select`,
`action`, `key`, `type`, `pass`, `swallow` or `onboarding`). Its `sequences` are presses and
releases in order, for what depends on timing (pairs). Each harness's own tests replay them. They
are test data, not code.

## 10. Settings files and the command line

### 10.1 One folder of files

Every platform has the same folder (`~/Library/Application Support/bada`, `%APPDATA%\bada`,
`~/.config/bada`):

| File | What |
|---|---|
| `settings.json` | `bada-settings/2`: `engine`, `keys`, `harness` (§8, §9) |
| `layouts/*.json` | The user's layouts: new ones, and changed copies of stock ones, which replace them (§7.6) |
| `dictionary.json` | The user dictionary (§11) |
| `history/` | The last few `settings.json`, for undo |

- **The running input method watches the folder and reloads.** The settings page and the command
  line write files (a temporary file, then a rename) and never talk to it directly. So an input
  method that isn't running can still be set up, and nothing needs a socket.
- A file that can't be used changes nothing (§8.2): the harness keeps what it had, and publishes
  the problems (§6.7). `settings.json` is one unit: an error in any of its three parts, and none of
  it is taken. The harness checks `keys` itself, with the engine's codes (`bad-value`, `unknown-field`,
  `unknown-action`: errors). A binding, `cycle` or `henkan_enters` that names a profile the settings
  don't have is only a warning (`unknown-profile`): that action doesn't apply (§9.2), and the file
  is still taken. A file that isn't JSON is `bad-json`.
- **One settings page** serves every harness, driven by two schemas: `P_describe`'s
  `settings.schema` for the `engine` part, and this standard's for `keys`. It reads and writes
  nothing itself: a **host** does, with one interface on every platform. On macOS, Windows and
  Linux the host is the command line (`bada settings`, §10.2), which serves the page on
  `127.0.0.1` to a window of the input method's own (macOS) or to the browser; on the web it is
  the harness, and the folder is the browser's storage.
- **The host checks before it writes**: the `engine` part by a dry `settings` event (§6.6), `keys`
  as a harness does (§9), and a file with an error-level problem isn't written. Every write of
  `settings.json` keeps the file it replaced in `history/`.

### 10.2 `bada`, the command line

A harness with no text field: it links the engine (to check what it writes, with `dry: true`), reads
and writes the folder above, and installs releases. Everything the settings page can do, it can do.

| Command | Does |
|---|---|
| `bada version` | The command line's, the engine's and the standard's versions, and the installed input method's |
| `bada update` | Installs the newest release from `team-mirae/bada` on GitHub. `--check` only looks |
| `bada settings` | Opens the settings page (§10.1): serves it on `127.0.0.1`, at an address that carries a token made for the run, and stops when the page is closed |
| `bada settings show` \| `get <path>` \| `set <path> <json>` \| `validate` \| `import <file>` \| `export` \| `undo` | The whole file, or one value by path (`engine.henkan.profiles[0].layouts`) |
| `bada profile list` \| `add <id> --layouts …` \| `remove <id>` \| `move <id> <n>` \| `set <id> …` | The profile list (§8) |
| `bada keys list` \| `bind <key> <action>…` \| `unbind <key>` \| `available` | The key table (§9). `available` is what the harness detects now |
| `bada layout list` \| `show <id>` \| `add <file>` \| `set <id> <path> <json>` \| `remove <id>` \| `reset <id>` \| `copy <id> <new-id>` \| `validate <file>` \| `missing <id>` | Layouts, stock and the user's. `reset` restores a stock layout. `missing` prints what a layout can't type on JIS, and its free slots (§7.4). A change to a stock layout is refused without `--modify-stock`, and the answer says why (§7.7); with it, and for any new id, the warning is in the output |
| `bada dict show` \| `add <word>` \| `remove <word>` \| `import <file>` | The user dictionary |
| `bada type --profile <id> "<keys>"` | Types on the engine with no text field and prints the result: a way to test a profile or a custom layout |
| `bada state` | The published state (§6.7), read-only |
| `bada schema [settings\|layout\|keys]` | The JSON Schemas |

- **For agents**: every command takes `--json` and answers on stdout, problems included, with a
  non-zero exit code on error; nothing asks a question.
- **It changes settings, not typing.** The command line can't switch mode or type into an app: the
  state stays read-only from outside.
- **`bada update` replaces a program that sees every key, so: only when asked.** The input method
  never contacts GitHub by itself, not even to look: `bada update` and `bada update --check` are the
  only ways. The download is checked against a signature whose key is in the installed build.

## 11. Data and dictionary

**Data.**

- Buffers are **named**: `P_data_new` takes `{name, ptr, len, flags}` for each. The harness passes
  what it has; `P_describe.data` lists what the engine knows. A missing optional buffer turns its
  feature off; an unknown name is ignored. Adding a buffer breaks no harness.
- With `P_BORROWED` the engine may use a buffer in place, for the blobs `P_describe` marks
  `borrow`. A harness that maps the data file and passes its blobs borrowed lets the OS share them
  between processes.
- The engine's own build writes one **pack**: a file of names and blobs, which a harness maps and
  passes blob by blob. **The pack's byte layout is not in v2.0 yet**: it is to be written here
  before the first release, with the contract layer.
- A layout file refers to a buffer by its name (§7.5).

**Dictionary.** A `dictionary` event sets the session's user dictionary: `value` is the dictionary
object, or null to clear it, and the reply has the summary (§6.6). There is no process-wide state
and no string the caller frees. The format is v1's, unchanged: `bada-dictionary`, version 1
([reference/dictionary-contract.md](reference/dictionary-contract.md), with its schema and an
example).

**Not in v2.0**: a path for the engine to hand learned words back to the harness.

## 12. Identifiers

bada is a new input method under `com.teammirae`, publisher **Mirae Research**. Every id, folder and
GUID is its own, so it installs beside bada ime and its ports, and neither reads or writes the
other's files.

| What | bada |
|---|---|
| Engine id (`P_describe`) | `com.teammirae.bada` |
| Symbol prefix | `bada` (`bada_send`, `bada_describe`) |
| Harness ids | `com.teammirae.bada.mac`, `.windows`, `.linux`, `.web`, `.cli` |
| macOS bundle | `com.teammirae.inputmethod.Bada` (`inputmethod` must be in the id) |
| macOS modes | `com.teammirae.inputmethod.Bada.Muhenkan`, `.Henkan` |
| macOS connection, keylayouts, login agent | `com.teammirae.inputmethod.Bada_Connection`, `.keylayout.*`, `.login` |
| macOS state | The file `~/Library/Application Support/bada/state.json`, and the distributed notification `com.teammirae.inputmethod.Bada.state` |
| Linux state | The file `$XDG_RUNTIME_DIR/bada/state.json`, and the D-Bus signal `Changed(s)` of the interface `com.teammirae.Bada.State` at `/com/teammirae/Bada`, with the JSON |
| IBus component | `com.teammirae.Bada` |
| Windows CLSID and profile GUID | New ones |
| Windows registry | Registration only (TSF needs it); settings are files (§10.1) |
| Windows version resource | ProductName `bada`, CompanyName `Mirae Research` |
| Windows state | `Local\com.teammirae.bada.state.v2.{CLSID}`, message `com.teammirae.bada.state` |
| Files | A folder named `bada` (§10.1) |
| Layouts | Slugs (§7.1) |
| Profiles | The user's own ids (§8.1) |

On a Mac, the code-signing identifier must equal the bundle id, or macOS refuses the input method's
connection and nothing types.

## 13. Conformance

The checklist is also a program: `tools/conformance` loads any engine and replays transcripts on it,
and `crates/compat/tests/conformance.rs` runs the engine's part through the C ABI.

**An engine**

- [ ] The eight functions are exported under one prefix.
- [ ] `P_describe` answers before anything is loaded, with `abi.major` 2, and lists `key`, `select`,
      `flush`, `reset` and `state` among its events.
- [ ] Every reply is valid JSON with `ok`. An unknown key code gives `consumed: false`, never a crash.
- [ ] An unknown event type answers `unknown-event`; invalid JSON answers `bad-json`; an event that
      needs an engine and got none answers `needs-engine`. Each changes nothing.
- [ ] `select` commits what was staged when the mode or the profile changes, and nothing when
      neither does. An id the settings don't have answers `unknown-profile`.
- [ ] `flush` leaves nothing staged, and `reset` leaves nothing staged and commits nothing.
- [ ] `staged` is the `commit` a `flush` sent instead would return, character for character.
- [ ] `state` changes nothing: sent any number of times, it answers the same.
- [ ] `mode` and `profile` are echoed as last selected, and never changed by the engine.
- [ ] With a preedit, a key the engine doesn't use commits it and returns `consumed: false`.
- [ ] An action it doesn't have answers `unknown-action` and does nothing else.
- [ ] A reply lives until the next `P_send` on the same engine; arguments are copied, borrowed
      buffers aside.
- [ ] Engines of one session, and sessions on one Data, work concurrently on different threads.
- [ ] Nothing unwinds across the boundary: a panic answers `panic`, with the engine reset.
- [ ] Settings with an error-level problem change nothing, and the reply lists the problems. Invalid
      settings never reset to defaults.
- [ ] `dry: true` changes nothing.
- [ ] More profiles than it takes are refused, never truncated; it takes at least 32.
- [ ] It asks for `around` only of a harness that has it, and writes `dir: "rtl"` only to one that
      has `rtl`.
- [ ] Positions are in Unicode scalar values: `preedit.caret`, and segment lengths that add up to the
      length of `preedit.text`.

**An engine with layouts** (`custom-layouts`)

- [ ] A layout with an error-level problem isn't loaded; a stock one falls back to the shipped file.
- [ ] A layout with a stock id replaces the stock one, with `stock-modified`; removing it brings the
      stock one back.
- [ ] A typing key a layout doesn't define types its JIS character (§7.2), and ⇧0 nothing.
- [ ] A key pressed with ⌃ or ⌘ is passed on with no `types`.

**A harness**

- [ ] It refuses an engine with another `abi.major`.
- [ ] It sends `select` after `P_new`, and when mode or profile changed while another field had focus.
- [ ] It applies every reply as §6.3 has it, `ask` once and never deeper.
- [ ] It never sends a language key or Caps Lock as a `key` event.
- [ ] It honours `types` (§6.4), consulting the OS's layout only to see whether the OS would type the
      same text.
- [ ] Its key table passes every case of [cases/key-table.json](cases/key-table.json).
- [ ] It sends `flush` before the field changes under a click or an edit.
- [ ] On `panic` it deletes the marked text and closes the menu.
- [ ] It publishes nothing typed in a password field, and takes nothing back from readers of the state.
- [ ] It keeps the mode's profile as an id string, never an index.

**A layout file**

- [ ] It validates against [schemas/layout.schema.json](schemas/layout.schema.json).
- [ ] Its `kind` is `muhenkan` exactly when its `read` is `direct`, and it has the part its `read`
      names and no other.
- [ ] Every role is one its way of reading has, and every option it uses is declared in it.

## 14. Where bada is more specific

Behaviour v2 doesn't require, but that bada has and a bada-like engine should follow. It is bada ime
0.3.1's (its [spec](reference/bada-ime-spec.md), §6), kept where the design is silent:

- **Henkan keys**: Space, Enter, Esc and other keys commit what shows, then act; **Esc commits**, it
  doesn't cancel. ⇧Enter commits and does nothing else. Tab and ↓ are the next entry, ⇧Tab and ↑ the
  previous; with nothing staged they go to the app. Backspace takes back the last staged key.
  ⌥Space commits, then types a full-width space.
- **The menu**: typing lands on entry 0; no wrap; committing commits the entry shown. A picked entry
  is fixed by typing on, and the layout that was fixed sticks until a row is chosen by hand again.
- **Muhenkan**: the engine consumes only the keys it types itself: the variants of §7.5.2, and a
  typing key whose slot is empty. In the strip of variants, 1–9 and 0 pick, the same ⌥ + letter
  again or the arrows choose, Enter commits, Esc and Backspace cancel, and any other key keeps what
  shows and goes on.
- **Search layouts**: with only search layouts in a profile, Space is part of the search, and ⇧Space
  commits and types a space.
- **F6–F10** in henkan are the engine's, per profile (§8.1): swallowed unless bound. In muhenkan
  every F key goes to the app.
- **Data**: bada's buffers are named `ko-words`, `en-words`, `ja-words`, `ja-kana-native`,
  `ja-common-words`, `kanji`, `hanja`, `ko-jamo-4gram`, `ja-shortcuts`, `en-ko`, `symbols`, `emoji`,
  `unicode-names` and `key-models`.

## 15. From v1

| v1 | v2 |
|---|---|
| 5 required functions and 9 optional, found with `dlsym` | 8 functions, all required; everything else is an event type, listed in `P_describe` |
| `P_key`, `P_set_mode`, `P_set_profile`, `P_flush`, `P_host_selection`, `P_state`, `P_set_settings`, `P_dictionary_load` | `P_send` with `key`, `select`, `flush`, `text`, `state`, `settings`, `dictionary` |
| `Lang1`, ⇧`Lang1`, ⌃`Lang1` sent as keys | `action` with `menu.next`, `menu.prev`, `fix` |
| Modes 0 and 1 | `"muhenkan"` and `"henkan"` |
| Three profiles, `mac-kana`, `henkan`, `katakana`, with links; an unknown name falls back | A list of any number, with the user's ids; an unknown id is an error |
| Settings sent to every engine; invalid JSON resets them | Settings are the session's; invalid settings change nothing |
| `P_data_new(b0, n0, …)`, positional, copied | Named buffers, which may be borrowed |
| `P_dictionary_load`, process-wide, with a string the caller frees | A `dictionary` event on the session |
| `{}` on a bad event or a panic | `ok: false` with a code |
| `preedit` a string; `menu`, `notes`, `japanese`, `zero`, `selected` side by side | `preedit` with a caret and segments; `menu.rows` |
| `P_state` after every call | The reply is the state |
| `need_selection` | `ask`, also for the text around the caret |
| A JIS legend table in each host | `types`, from the engine's one table |
| Integer layout ids; layouts in code | Slugs; a layout is its `bada-layout/3` file |
| The language-key rules in each host's code and settings | One key table in one format, with cases |
| `P_catalog`, free-form | `P_describe`, with a schema |
| `bada-compatible-state/1` | `bada-compatible-state/2` |
