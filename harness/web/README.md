# harness/web

The web harness, `com.teammirae.bada.web`: bada's engine compiled to WebAssembly behind a text
field in a page. It is for trying bada out. It isn't an input method: it types only into its own
field.

It follows [bada-compatible v2](../../docs/bada-compatible-v2.md). The section numbers below are
that document's.

## Build, test, serve

```sh
node harness/web/build.mjs       # harness/web/dist. Needs Rust with the wasm32-unknown-unknown target
node harness/web/test/all.mjs    # every test below, on the wasm just built
node harness/web/serve.mjs       # http://localhost:8000 (a port may follow), this machine only
```

`python3 -m http.server -d harness/web/dist` serves it as well. There are no npm packages and no
framework: plain modules.

`vercel.json` is for a Vercel project whose root directory is this folder: it adds the wasm target to
the Rust that Vercel's build image has, runs `build.mjs`, and serves `dist`. Without it Vercel serves this folder as it is,
which has no page at its root.

| Test | Checks |
|---|---|
| `test/keytable.mjs` | Every case and sequence of [docs/cases/key-table.json](../../docs/cases/key-table.json) on `src/keytable.js` (§9.6), and what the module decides beyond them |
| `test/field.mjs` | The text field model against §6.3, rule by rule, with replies written by hand |
| `test/conformance.mjs` | The transcripts of [tools/conformance](../../tools/conformance) (`any-engine.jsonl`, `bada.jsonl`) on the wasm: raw through `bada_send`, then again with every reply applied to a text field model, whose text is checked after each |
| `test/smoke.mjs` | Typing through `src/harness.js`, the code the page uses: muhenkan, Korean 2-set, romaji, the key table, profiles, settings, layouts, the dictionary, the state, a trap in the engine |

No test opens a browser.

## What is where

| | |
|---|---|
| `wasm/` | A crate of its own (not in the repository's workspace). It links the engine and adds `bada_web_alloc` and `bada_web_free`, so a page can put bytes in the module's memory. The eight functions of §3 are the engine's own exports |
| `src/engine.js` | The eight functions from JavaScript: one session, a handle per text field, JSON in and out. It refuses an engine whose `abi.major` isn't 2 |
| `src/field.js` | The text field model: text, selection, marked range. `apply` is §6.3 |
| `src/keytable.js` | The key table (§9), with no DOM: bindings, actions, pairs, Caps Lock, the presets, and a check of the `keys` part |
| `src/settings.js` | The settings file and where things are kept in local storage |
| `src/harness.js` | The harness without its page: `press` of §6.3, the key table carried out, settings, layouts and dictionary fed to the session, the published state |
| `src/app.js`, `index.html`, `style.css` | The page: the browser's keys in, the model mirrored into a `<textarea>`, the marked text, the menu, the state panel |
| `src/settings-ui.js` | The settings dialog (JSON editors) and the onboarding |
| `src/keyboard.js`, `keyboards.js` | The keyboard drawn on the page, with the legends of the layouts in use |
| `src/load.js`, `build.mjs` | Loading and building; `manifest.json` says which wasm, data and layout files a build has |

## How it works

**The engine.** One session for the page and one engine for its text field. The page sends
`select` after `bada_new`, and everything goes through `bada_send` as JSON. WebAssembly can't
unwind, so the engine's own panic guard never runs here: a panic is a trap. `engine.js` catches it,
starts a new instance with what the session and the field were last given, and answers `panic`
(§6.2), so the marked text and the menu go.

**Replies** are applied as §6.3 has them, on the model. The marked text's segments are drawn each in
its way: `live` underlined, `fixed` with a thin line, `picked` highlighted. A click on a menu row
sends `pick`, then `flush`. A click in the text, an edit by the browser and a blur send `flush`
first.

**Keys passed on** (§6.4). When `types` is a string the key types that text. The browser types it
when `KeyboardEvent.key` is the same text, and otherwise the page inserts it and keeps the key from
the browser. So the OS's layout doesn't matter. With `types` null the browser has the key, but for
Space and Enter, which the page types itself.

**The key table** is `keys` of the settings file. With no `keys.bindings` it is a preset of §9.5:
by default the one for the language keys the page has seen (`Lang1`, `Lang2`: JIS on a Mac;
`Convert`, `NonConvert`, `KanaMode`: JIS on a PC), and before any was seen the platform's. The
*Keys* list under the text field chooses another, or "No language keys" (Caps Lock toggles).

**Caps Lock** is the page's own lock: a page can't read or set the OS's reliably. Unbound, the key
turns the page's lock on and off (§9.4).

**Settings.** One `bada-settings/2` object in local storage. Its `engine` part goes to the engine
with a `settings` event, `keys` is the key table, and `harness["com.teammirae.bada.web"]` holds the
page's own two choices (`keyboard`, the key preset's keyboard, and `board`, the one drawn). The dialog's editor checks all three parts, lists
every problem, and takes the file only when none is an error. Import and Export move it as a file.
A new install has no layout and no profile, and gets the onboarding (§8.3).

| Local storage key | Holds |
|---|---|
| `bada.settings` | The settings file, as text |
| `bada.layouts` | The user's layouts, `{id: layout}`. Each is fed with a `layout` event before the settings |
| `bada.dictionary` | The user dictionary, as text, fed with a `dictionary` event |
| `bada.web.last` | The mode and profile the page was left in (ids, never indexes) |
| `bada.web.seen` | The preset of the language keys the page has seen |

**The state** (§6.7) is in the small panel under the text field, and published as
`bada-compatible-state/2` in `<script id="bada-state">`, with a `bada-state` event on `window`
when it changes. Nothing is read back from either.

## Data

**The engine loads no data yet**, so the page passes no buffers: no kanji conversion, no word
ranking, and the search layouts find nothing. The place for it is ready and marked:

1. `DATA` in `build.mjs` names each buffer's file under `engine/data`. The build gzips it into
   `dist/data` and lists it in `manifest.json` under the buffer's name.
2. `src/load.js` fetches and unzips what the manifest lists.
3. `Engine.dataNew` in `src/engine.js` passes them to `bada_data_new` as named buffers (§11),
   borrowed where `bada_describe` says `borrow`.

## A key of this harness's own

**⇧Tab is the toggle between muhenkan and henkan while no menu is open** (one row or none), as
`mode.toggle` goes (§9.2): into `henkan_enters`' profile, or the onboarding with no profile. A
browser's keyboard often has no language key, and this gives it a way into henkan without the
mouse. With a menu open ⇧Tab is the engine's, the entry before. No other harness has this, and it
isn't in the key table: a binding on `shift+Tab` in the settings comes first, so
`{"on": "shift+Tab", "do": ["pass"]}` gives the key back to the browser. Without that, ⇧Tab no
longer moves the focus back out of the text field.

## Where the standard is silent, this harness decides

- **A profile action with no profile at all** opens the onboarding, `profile.next`, `profile.prev`
  and `profile.last` included.
- **`cycle` skips a profile that doesn't exist.**
- **Whether a pair is bound** is asked of the bindings that hold now (`when`).
- **A bound modifier** (`MetaRight`) runs when it is pressed and released with no other key between.
- **A held key**: what isn't the table's repeats; a binding runs once, but for `pass`.
- **The mode's own buttons** enter a profile directly, or `henkan_enters`' profile.
- **A warning in `keys`** (a profile or layout that isn't there) is taken and shown; an error
  (a shape the schema doesn't have) refuses the file.

## Not done

- No data (above), and no borrowed buffer has been passed for real.
- No settings UI beyond the JSON editors: the shared settings page doesn't exist yet.
- `rtl` is not among the harness's features: a `<textarea>` can't draw one run right to left.
- Not tried on a phone, in Safari or in Firefox.
