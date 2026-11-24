# bada: the next generation of bada ime

Design, 2026-11-24. Nothing here is built. Every decision it raised has been settled by the owner
and is listed in §14.

**Words.** The *engine* turns keys into text. A *harness* is the platform program around it: it
gets keys from the OS, feeds the engine, puts the text in the app and draws the menu (v1 calls it
the host, wrapper or shell; §2 keeps v1's word). **The harness decides what language keys and Caps
Lock do; the engine only sees what its harness feeds it.**

- [1. What was read](#1-what-was-read)
- [2. Pitfalls of bada-compatible v1](#2-pitfalls-of-bada-compatible-v1)
- [3. The proposal in one page](#3-the-proposal-in-one-page)
- [4. The ABI](#4-the-abi)
- [5. Events and replies](#5-events-and-replies)
- [6. Layouts](#6-layouts)
- [7. Henkan profiles: n of them](#7-henkan-profiles-n-of-them)
- [8. Keys the harness decides](#8-keys-the-harness-decides)
- [9. Settings, files and the CLI](#9-settings-files-and-the-cli)
- [10. Data and dictionary](#10-data-and-dictionary)
- [11. One repository](#11-one-repository)
- [12. Identifiers: com.teammirae](#12-identifiers-comteammirae)
- [13. Order of work](#13-order-of-work)
- [14. Decisions](#14-decisions)

## 1. What was read

| Repository | At | State |
|---|---|---|
| bada-compatible | `f5596c4` main (2026-11-13) | Was on a deleted branch, 3 commits behind; now on main. 27 tests pass |
| bada-ime | `ed5cc16` main, 0.3.1 | Up to date. `origin/keymodel-idle-shift` (2026-11-19) is unmerged, ranking only |
| pado-ime | `dcdb134` main | Was 8 behind; fast-forwarded. `engine/` identical to bada-ime's |
| ara-ime | `cfcde01` main | Cloned. `engine/` identical to bada-ime's |
| jis-moonshot | `00a90bf`, 0.7.0 | Up to date |
| jis-korean | `5c41b36`, 0.4.0 | Up to date |

Also looked at, because the contract touches them: bada-windows, bada-linux, superkeyboard,
bada-enigma (local copies, not refreshed).

## 2. Pitfalls of bada-compatible v1

Each has its evidence. They are grouped by cause, and the groups are in the order they hurt.

### A. Each host writes its rules alone, and they have drifted

That the host decides is right. The trouble is that every host decides in its own code.

1. **The language-key table (§7.4) is written seven times**: `bada-compatible/src/compat/host.rs`,
   bada-enigma's copy of it, bada-ime `mac/Sources/ProfileKeys.swift` + `InputController.swift`,
   bada-ime `web/src/app.js`, bada-windows `src/core/host.cpp`, bada-linux `src/core/host.cpp`,
   superkeyboard `src/session.rs`. They already disagree:
   - On Windows and Linux the home profile is `henkan`, not `mac-kana` ("the owner's adaptation",
     pado `windows/host/docs/spec.md:120-137`); `mac-kana` has no PC key there, **[decide]** in that spec.
   - Linux eats any ⌃ + profile key, so v1.3's "fix what shows" can never be sent
     (bada-linux `host.cpp:145`), though the engine has it.
   - `kana_in_profile` and `profile_order` work on the Mac and the web. pado's settings page shows
     them (`windows/settings/page/ui.js:192-201`) and nothing in the Windows host reads them.
2. **Caps Lock policy likewise**: toggle / transient / respect and UPPER_SNAKE are in the Mac shell,
   again in bada-windows (`host.cpp:100-181`) and again in bada-linux (`host.cpp:109-178`, where
   `caps_wanted` is computed and dropped).
3. **Smaller rules, each in its own host**: a list of terminal apps where ⌥ passes (Mac
   `InputController.swift:108`), per-app "off" lists (Windows registry `Apps\<exe>\Off`, Linux
   `off_in`), a JIS legend table for passed-on keys in all three, a Right Alt tap in the Linux host.
4. **All three hosts rebuilt the same replay.** Mode, profile and settings are sent to each engine,
   and there is one engine per text field: three ordered calls on every activation on the Mac, a
   generation counter on Linux (`session.cpp:64-73`), a TSF global compartment on Windows that stores
   `1 + mode + 2 × index in kProfiles`: a fixed table index, which cannot hold a fourth profile.

### B. Profiles

5. **Exactly three, named after one keyboard's keys**: `PROFILES: [&str; 3]` in the engine, and the
   names `mac-kana`, `henkan`, `katakana` in the standard (§7.4, §8.1) and in every host.
6. **An unknown name silently becomes `mac-kana`** (§8.1), so a typo or a newer host's profile is invisible.
7. **`link` exists only because the three slots always exist.** It needs loop and dead-end rules, and
   the top-level `enabled` is also `mac-kana`'s own list: one fact in two places.
8. **Key → profile is stored as macOS key codes** (`profile_keys`, HID keys as `0x70000 | usage`):
   a settings file doesn't move between platforms.
9. **The host's requests reach the engine disguised as a key**: "next entry", "previous" and "fix what
   shows" are `Lang1`, ⇧`Lang1` and ⌃`Lang1`. A v1.2 engine reads the third as the first.
10. bada spec §10's open question 3 (what HYPER+かな and MEH+かな go to) can't be answered with fixed slots.

### C. The contract follows an implementation

11. **The reference engine doesn't use the contract layer and isn't tested against it.** bada-ime has
    its own `engine/src/ffi.rs`; `tests/conformance.rs` links only the template's module. Result:
    bada's ffi has no `catch_unwind` and no `panic = "abort"`, though §4 and §10 forbid unwinding. A
    panic aborts the process, and on Windows that process is the app being typed in (the engine is
    loaded into every app).
12. **The template is copied, not depended on** ("Copy this repository"): bada-enigma carries its own
    `src/compat/`. A fix to the layer reaches no one.
13. **Versions are prose.** An engine can't be asked what it speaks (`VERSION`, `MINOR` are Rust
    constants, not exported). v1.3 says a host "should send [⌃Lang1] only to an engine that has it":
    it can't tell. The document itself calls ⌃Lang1 both v1.2 and v1.3 (§1, two rows of §7.4,
    `key.rs`). Hosts lag silently: `direction` (v1.2) isn't parsed by the Mac, Windows or Linux host.
14. **The engine is copied into forks.** pado-ime and ara-ime each carry `engine/` (224 data files)
    and the docs, kept equal by a daily merge workflow, and re-export it (`pub use bada::ffi::*`).

### D. Output and events

15. **Parallel arrays and sentinels**: `menu` / `notes` / `japanese` by index, "empty when no row has
    one"; `selected` "past the end: none"; `zero` arithmetic for negative entries.
16. **`japanese: bool[]` is one language's flag.** The Sino range (30–49) will need more. Linux parses
    it and ignores it.
17. **The preedit is one string**: no caret, no segments. 0.3.1's "fix what shows" makes a fixed part
    and a live part that no host can draw apart; Windows and Linux each invent an attribute from
    `selected != zero`.
18. **No way to pick a row** by pointer or touch (both pado and ara specs say so), and **no reset**:
    flush always commits, so hosts call `P_flush` and throw the commit away (Windows
    `text_service.cpp:306`, IBus focus-out).
19. **Only the selection can be asked for**, one round. JIS Moonshot had
    `jk_host_edit(before, selected, after)`.
20. **No repeat flag on key events** (JIS Korean's ABI had `JK_MOD_REPEAT`); the Mac host filters
    repeats for language keys only.
21. **Errors are invisible.** A bad event and a panic both answer `{}`. Invalid settings JSON resets
    every setting to its default (bada-ime `engine/src/ffi.rs:201`, the template's
    `src/compat/ffi.rs:150`): a host bug or a truncated file undoes the user's setup.
22. **State costs a second call per key** (§12.1). IBus calls and parses `P_state` on every preedit
    update just to learn `staged`.

### E. Settings, catalog, data

23. **One opaque blob mixes engine and host fields**, replace-all, no schema. `kana_in_profile` has
    already changed shape once (`Settings.swift:68`) without a version.
24. **The catalog is free-form, so it isn't used.** Neither bada-windows nor bada-linux calls
    `P_catalog`. pado's settings app links the engine crate and reads `bada::layout::CATALOG`
    directly; Linux has no settings UI. There are three hand-written UIs (AppKit 751 lines, web,
    pado's page). "Any wrapper that runs bada's engine can run yours" holds for typing only.
25. **`P_data_new` is positional.** bada has 11 buffers; the count and order are spelled out in every
    host (11 nested `withUnsafeBytes` on the Mac, a 17-case arity switch on Windows and on Linux, the
    manifests), and one buffer is a concatenation each host must build (`#! brands`). Adding a
    buffer breaks them all. `key-models.bin` is compiled in instead: two ways to ship data.
26. **Buffers must be copied.** On Windows that is about 140 MB and 0.6–1.2 s in every process
    (pado `docs/windows.md:101-104`, which names the fix: read from a mapped file).
27. **The user dictionary is process-global** (`static CURRENT`), the only caller-freed string in the
    ABI, loaded once on Linux and its summary dropped by two hosts.

### F. Identity

28. Ids have several roots: `me.sjun.inputmethod.Bada` (Mac), `bada-ime/` (the Mac dictionary folder),
    `bada-windows` (registry, paths, shared memory), `me.sjun.BadaLinux` (D-Bus, IBus). §12 below.

## 3. The proposal in one page

Kept from v1: a headless engine with a C ABI, JSON in and out, no I/O, replies owned by the engine
until the next call, two modes (muhenkan, henkan), W3C key codes, "commit, then act", and the
harness owning the language keys and Caps Lock.

| | bada ime 0.3.1, bada-compatible v1 | bada |
|---|---|---|
| Name and home | bada-ime, plus bada-compatible, bada-windows, bada-linux and two forks: all left as they are | **bada**, a new input method: one repository, `team-mirae/bada`, MIT, with the engine, every harness and the CLI (§11) |
| Layouts | 14, integer ids, most of them Mirae Research's own | **11 stock layouts** with slug ids, each entirely a JSON file the user can change; the user's own layouts beside them (§6) |
| Symbols | Per layout; a legend table in each harness | The JIS legends in every layout, unless the layout says otherwise (§6) |
| Henkan profiles | 3 fixed names with links | **An ordered list of n** (§7) |
| What the harness asks of the engine | Three calls, and `Lang1` in disguise | `select` (mode, profile) and `action` (by name) (§5) |
| The harness's key rules | Code in each harness, each with its own settings | Still each harness's own code, reading **one key table** in one format; a **toggle**; any chord once the OS allows it (§8) |
| Where settings live | User defaults, the registry, JSON | The same folder of JSON files on every platform, edited by the settings page or the **`bada` CLI** (§9) |
| Functions | 5 required + 9 probed with `dlsym` | **8**, all required; everything else is an event type (§4) |
| Versions and options | Document footnotes | `P_describe`, and the harness's features in `P_session_new` (§4) |
| Output | Flat fields, parallel arrays | A **snapshot**: structured preedit and rows, staged (§5) |
| Errors | `{}` | `ok: false` with a code; bad settings change nothing (§5) |
| Data | Positional copies | **Named buffers**, may be borrowed from a mapped file (§10) |
| Ids | `me.sjun`, project names: unchanged | `com.teammirae` for the new input method; publisher Mirae Research (§12) |

Two rules are written down that v1 only implied:

- **The engine only sees what its harness feeds it.** It gets keys to read and requests by name. It
  never learns which key the user pressed to change mode or profile, what app has focus, or
  whether Caps Lock is on.
- **A reply only ever answers an event.** No timers, no idle events, nothing re-ranked while the
  user pauses.

## 4. The ABI

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

- **Three objects.** *Data*: dictionaries and models, built once, immutable, shareable between
  threads. *Session*: what the harness has fed that every text field shares: the settings, the
  custom layouts and the user dictionary; one per process in an input method, one per run of the
  CLI. *Engine*: one text field: its mode, its profile, its staged keys.
- **Mode and profile are the harness's.** It tells an engine with one `select` event: after `P_new`,
  and when they changed while another field had focus. That replaces v1's three ordered calls; the
  settings no longer need re-sending, since they are the session's.
- `engine_or_null`: `settings`, `layout` and `dictionary` are session events and need no engine.
  Everything else is about one text field.
- Memory and threads as v1: a reply lives until the next `P_send` on the same engine (or on the
  session, when the engine is NULL). An engine is used by one thread at a time; a session locks itself.
- The symbol prefix stays (`bada_send`, `shout_send`), so one library can still hold several IMEs.
- `P_describe` answers before anything is loaded:

```json
{"abi": {"name": "bada-compatible", "major": 2, "minor": 0},
 "engine": {"id": "com.teammirae.bada", "version": "1.0.0", "name": {"en": "bada", "ko": "바다", "ja": "バダ"}},
 "events": ["key", "action", "select", "pick", "text", "flush", "reset", "state", "settings", "layout", "dictionary"],
 "features": ["segments", "around", "custom-layouts"],
 "keyboards": ["jis"],
 "data": [{"name": "ko-words", "required": false}, {"name": "kanji", "required": false, "borrow": true}],
 "layouts": [{"id": "ja-kana", "name": {"en": "Japanese - Kana"}, "kind": "henkan",
              "actions": [{"id": "katakana", "label": {"en": "Katakana"}}], "options": {}}],
 "actions": ["menu.next", "menu.prev", "fix", "width.full", "width.half"],
 "settings": {"format": "bada-settings/2", "schema": {}, "defaults": {}}}
```

- The harness says what it can do in `init_json`:
  `{"harness": {"id": "com.teammirae.bada.mac", "version": "1.0.0", "features": ["segments", "pick", "around", "rtl"]},
  "settings": {…}, "layouts": [{…}]}`. An engine doesn't ask a harness for what it lacks. A harness
  refuses an engine with another `major`.
- A new event type, reply field or feature is a minor version. A harness never probes with `dlsym`.

## 5. Events and replies

### 5.1 Events

| `type` | Needs an engine | Fields | v1 |
|---|---|---|---|
| `key` | yes | `key` (W3C code), `shift` `ctrl` `alt` `meta` as v1; new, optional: `repeat` | `P_key` |
| `action` | yes | `name`: `menu.next`, `menu.prev`, `fix`, or a layout's action (`ja-kana.katakana`) | `P_key` with `Lang1`, ⇧`Lang1`, ⌃`Lang1` |
| `select` | yes | `mode`, `profile` (a profile id of §7). Commits what's staged if either changes | `P_set_mode`, `P_set_profile` |
| `pick` | yes | `row` | none |
| `text` | yes | `selection`, and `before` / `after` when asked | `P_host_selection` |
| `flush` | yes | `cause`: `blur`, `click`, `edit`, `harness` | `P_flush` |
| `reset` | yes | Drops what's staged; commits nothing | none |
| `state` | yes | Nothing. Changes nothing | `P_state` |
| `settings` | no | `set` (replace) or `patch` (JSON merge patch); `dry: true` only checks | `P_set_settings` |
| `layout` | no | `value`: a layout file's object (adds or replaces a custom layout), or `id` with `value: null` (removes it); `dry: true` only checks | none |
| `dictionary` | no | `value`: the dictionary object, or null to clear | `P_dictionary_load` |

- **The harness sends the keys it wants read.** Language keys and Caps Lock don't arrive, as in v1
  (§7.4, §7.7). What v1 sent disguised as `Lang1` is now an `action`, by name, so an engine that
  lacks one says so (`unknown-action`) instead of doing something else.
- **No focus, app or field information.** What a password field or a terminal gets is the harness's
  choice of what to send.
- An unknown `type` answers `ok: false`, `error.code: "unknown-event"`, and changes nothing.

### 5.2 The reply

Every event answers with the same object: what to do now, and everything that shows afterwards.

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

| Field | Replaces | Meaning |
|---|---|---|
| `ok`, `error` `{code, message, path}` | `{}` | false: nothing changed. After a caught panic the engine has reset itself and says so (`code: "panic"`), so the harness clears the marked text |
| `commit`, `consumed` | the same | As v1 |
| `types`: string \| null | The harnesses' JIS legend tables | With `consumed: false` on a character key: what the key should type, by the layout (§6). The harness lets the OS type it when the OS's keyboard layout gives the same character, and inserts it otherwise |
| `replace`: `"marked"` \| `"selection"` | `replace_selection` | |
| `preedit.text`, `.caret`, `.segments` | `preedit` | `kind`: `live`, `fixed` (0.3.1's fix), `picked`. A harness without `segments` draws `text` |
| `menu.rows[]`: `text`, `entry`, `note`, `lang`, `layout` | `menu`, `notes`, `japanese`, `zero` | `entry` is the signed index (0 the top reading, −1 / −2 script forms). `lang` is BCP 47: the harness picks the font |
| `menu.selected`: integer \| null | `selected` past the end | |
| `menu.strip` | `horizontal` | |
| `mode`, `profile` | `P_state` | As the harness last selected them: echoed, never changed by the engine |
| `staged` | `P_state` | What `flush` would commit now. With the fields above, the last reply **is** the state; `state` only re-reads it |
| `ask`: `{"selection": "hanja"}` \| `{"around": {"before": 20, "after": 0}}` | `need_selection` | Answered with a `text` event, once |
| `dir` | `direction` | |

Applying it is v1's §7.1 with the new names. A harness publishes the reply as the state for
scripts, under the v1 rules (read-only, blank in password fields), in `bada-compatible-state/2`.

## 6. Layouts

### 6.1 Names

A layout's id is a slug: **`<group>-<name>`**, lowercase letters, digits and hyphens. The group is the
language (`en`, `ko`, `ja`: ISO 639-1) or `uni` for layouts that belong to no language. **Integer ids
are gone**: nothing in the settings, the ABI or the documents uses one.

### 6.2 The starting set

| Id | Shown as | Kind | From |
|---|---|---|---|
| `en-qwerty` | English - QWERTY | types directly | v1's 0 |
| `en-qwerty-us` | English - QWERTY (US) | types directly | New: QWERTY with a US keyboard's symbols (§6.4) |
| `en-dvorak` | English - Dvorak | types directly | New: Dvorak with its own symbol places (§6.4) |
| `en-colemak` | English - Colemak | types directly | New: Colemak with its own `; :` on P (§6.4) |
| `ko-2-set` | Korean - 2-set | composes | v1's 10 (KS X 5002) |
| `ko-bada-roman` | Korean - bada roman | composes, ranked | v1's 15: Korean typed as it is romanized, with its English dictionary and romaji shortcuts |
| `ja-romaji` | Japanese - Romaji | composes, converts | v1's 21 |
| `ja-kana` | Japanese - Kana | composes, converts | v1's 20 (JIS X 6002) |
| `uni-unicode` | Unicode - All Unicode | search | v1's 151 |
| `uni-symbols` | Unicode - Symbols | search | v1's 150, its symbols |
| `uni-emojis` | Unicode - Emojis | search | v1's 150, its emoji |

**Dropped**: Mirae Research's other layouts: 11 (JIS Korean legacy), 12 (Moonshot), 13 (bada), 14
(3-set JIS), 16 (bada plain), 18 (Korean Roman), 19 (Kana Korean), with their key models. **Kept**:
bada roman (15), as `ko-bada-roman` (owner, 2026-11-24), and with it what it uses: the
English–Korean dictionary (`en-ko/`: common words, the brands of 200 countries, their order), the
romaji shortcuts (`ja-shortcuts.tsv`), the jamo model that picks among unknown readings
(`ko-jamo-4gram.bin`) and its key model. Its spellings came from Korean Roman (18), which is
dropped: they are in bada roman's own file now. The key models of the other layouts that stay are
kept; `en-qwerty-us`, Dvorak and Colemak need theirs built by the same script.

**Left out**: `ko-3-set-unshifted` (세벌식 순아래; owner, 2026-11-24). It was worked out from
libhangul's keyboard file, which is LGPL-2.1, and this repository is MIT. A user can still add a
3-set as a layout of their own (§6.5).

**There is no default layout.** A new install has no henkan profile and no layout chosen: the user
picks them in the onboarding (§7.1).

### 6.3 Symbols: the JIS legends, whatever the OS thinks

Every layout types the JIS legend on every key it doesn't define itself, on every platform. The
keyboard layout selected in the OS is never consulted. This is what bada-windows and bada-linux do
today with their own tables (ろ types `\`, ⇧ `_`); the table moves into the engine, once, so a layout
file can override it and no harness carries one. A harness gets the character with the key
(`types`, §5.2).

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

All 32 ASCII symbols are there, and ⇧0 is the one empty slot.

**Shortcuts follow the QWERTY positions** (owner, 2026-11-24): a key pressed with ⌘ or ⌃ is passed
on as the physical key, with no `types`, whatever the layout puts there. So ⌘C is the key printed
C under Dvorak and Colemak too, as in JIS Korean.

A layout **overrides** a slot by listing it: Dvorak's symbol places; JIS kana's kana and 、。・「」;
romaji's ー 、 。 ・ 「 」.

### 6.4 The layouts on a JIS keyboard

Each layout's established keys go on the 48 JIS keys, and the rest is filled from the table above.
Where that left a character with no key, it takes a free slot (owner, 2026-11-24). The boards are
drawn in `docs/layouts.html` (also published as the artifact "bada JIS layouts"); `bada layout missing <id>` would
print the same check for any layout (§9).

| Layout | Its own keys | Had no JIS key, and where it goes | Still can't type |
|---|---|---|---|
| `en-qwerty` | none | | nothing |
| `en-qwerty-us` | A US keyboard's symbols, key for key by position: ⇧2 `@`, ⇧6 `^`, ⇧7 `&`, ⇧8 `*`, ⇧9 `(`, ⇧0 `)`, `- _`, `= +` on ^, `[ {` on @, `] }` on [, `\ |` on ], `; :`, `' "` on : | The US `` ` ~ `` key has no JIS typing key: **`` ` `` and `~`** on ろ (plain, ⇧) | nothing. ¥ stays `¥`, with `\|` on ⇧ as well as on ⇧] |
| `en-dvorak` | Dvorak's letters and its own symbol places: `' "` Q, `, <` W, `. >` E, `; :` Z, `[ {` on -, `] }` on ^, `/ ?` on @, `= +` on [, `- _` on : | **`@` and `` ` ``** on the `]` key (plain, ⇧); **`~`** on ⇧0; **`^`** on ⇧2; **`*`** on ⇧7 | nothing |
| `en-colemak` | Colemak's letters and its own `; :` on P | **`+`** on ⇧0 | nothing |
| `ko-2-set` | Jamo on the letter keys; ¥ is ₩ | | nothing |
| `ko-bada-roman` | The QWERTY letters, read as romanized Korean (`q` is ㅊ, `x` an initial ㅇ); `;` separates syllables and ⇧; types `;`; ¥ is ₩; ⇧0 is `+`; ⇧[ and ⇧] are 「 and 」; ⌥; is `·`; ⌥⇧ + a number or symbol key types its shifted JIS character, as v1 | | `{` and `}` on plain and ⇧: they are on ⌥⇧[ and ⌥⇧] |
| `ja-romaji` | `-` ー, `,` 、, `.` 。, `/` ・, `[` 「, `]` 」; digits and other symbols full-width (v1) | | nothing: the half-width forms are rows of the menu |
| `ja-kana` | Kana on all 48 keys; ⇧ for small kana, を and 、。・「」; ⌥⇧ + a number or symbol key (Alt+Shift off the Mac) for its shifted JIS character, full-width, as v1 | | The digits 1–9, the unshifted symbols, and anything half-width, as JIS kana always has |
| `uni-*` | none: letters, digits and space spell the search | | |

What follows from those choices:

- **`en-dvorak`**: ⇧2 and ⇧7 no longer type the `"` and `'` printed on them (both are on Q), and the
  `]` key types `@` and `` ` `` (`] }` are on the ^ key). ⇧ろ still types `_`, which is also on ⇧:.
- **`en-qwerty-us`** (owner, 2026-11-24): the keys no longer match the JIS legends printed on them.
  半角/全角 is not a typing key and keeps its default (full-width, ⇧ half-width, §8.4), which is why
  `` ` ~ `` moved to ろ.
- **`en-colemak`**: `:` is on two keys, ⇧P and its own. The JIS `;` key types o.
- **`ko-bada-roman`** is as bada ime 0.3.1's layout 15 has it (`docs/layouts/15-bada-roman.md` there).
  It is the layout with the most to move out of code (§6.5): its file already has the spellings
  (onsets, vowels, codas, the break), but its rules (the spelling ranks first; a dictionary reading
  is never first; capitals choose brand names; a shortcut may rank first) are 530 lines of Rust.
- **`ja-kana`** is as bada ime 0.3.1 has it: the harness sends `alt` and `shift` for the ⌥⇧ layer on
  every platform. In a profile with an English layout the digits and the other symbols come from
  that layout.
- Not taken: JIS Korean's forms of Dvorak and Colemak (its spec §3.6.4), which move whole JIS keys
  and so lose nothing, at the price of Dvorak's and Colemak's own symbol places.

### 6.5 A layout is its JSON file, all of it

**Everything that makes a layout what it is lives in its JSON file, and the user can change any of
it, the stock layouts included** (owner, 2026-11-24). The engine has no code for any one layout: it
has a few general ways of reading keys, and each is driven entirely by the file.

**bada ime 0.3.1 is not there.** Every v1 layout has a `docs/layouts/*.json` (`bada-layout/1`), but
those are for the renderer and the editor. The engine reads only one of them (layout 14's, compiled
in with `include_str!`, and only its keys' roles); every other layout, and every layout's rules, is
Rust code (`layout.rs`, `japanese.rs`, `search.rs`). Taking all of that out of the code and into
the files is the largest piece of engine work in this design.

What a `bada-layout/2` file holds:

```json
{"format": "bada-layout/2", "id": "ko-2-set", "name": {"en": "Korean - 2-set", "ko": "두벌식"},
 "lang": "ko", "kind": "henkan", "read": "hangul",
 "keys": {"KeyR": {"base": {"t": "ㄱ", "r": "c"}, "shift": {"t": "ㄲ", "r": "c"}},
          "KeyK": {"base": {"t": "ㅏ", "r": "v"}}},
 "hangul": {"vowels": {"ㅗㅏ": "ㅘ", "ㅜㅓ": "ㅝ"}, "finals": {"ㄱㅅ": "ㄳ", "ㄹㄱ": "ㄺ"},
            "double_tense": false, "final_moves_on": true},
 "actions": [{"id": "hanja", "do": "lookup", "data": "hanja"}]}
```

| Part | What it says | In v1 |
|---|---|---|
| `keys` | Each key's layers (`base`, `shift`, `alt_shift`) and each legend's role. A slot left out is the JIS table's (§6.3) | A table in Rust, but for layout 14 |
| `read` | Which general way of reading the keys: `direct`, `hangul`, `table` (romaji and the like), `kana`, `search` | The layout's own module |
| That way's tables | Hangul: which vowels join, which finals cluster, doubling, whether a final moves on. `table`: the spellings (`"ka": "か"`) and punctuation (`","`: `"、"`). `kana`: the voicing marks | Rust code; romaji's spellings were in the JSON but not read |
| `convert` | What turns the reading into candidates, and the data it uses, by name: `{"with": "kanji", "data": "kanji"}`; script forms (hiragana, katakana) | `japanese.rs`, `kanji.rs` |
| `search` | What is searched: a named data buffer (`"data": "emoji"`), or entries written in the file | `search.rs` |
| `actions` | The layout's named actions (F keys and `action` events), each one of the engine's general operations with its arguments | A constant in `engine.rs` |
| `words` | The word lists that rank its readings, by name (`"ko-words"`) | Fixed per layout in code |

- **What stays outside the file** is bulk data and the general machinery: the kanji dictionary, the
  word counts, the emoji and Unicode lists are named data buffers (§10), which a file refers to by
  name and a user can also replace; the conversion and composition algorithms are the engine's, the
  same for every layout that asks for them. A layout can hold small data itself (a search list, a
  spelling table); a new *algorithm* is still code.
- **The stock eleven are files like any other**, shipped with the engine. A file with a stock id in the
  user's `layouts/` folder (§9.1) **replaces** the stock one; deleting it brings the stock one back
  (`bada layout reset <id>`, or "Restore" in the settings).
- **Three ways in**, all the same event underneath (`layout`, §5.1): the settings page's layout
  editor (v1's `tools/editor` is the start of it); the C ABI, for any harness; the CLI (§9.2).
- The harness keeps the files and feeds them at start. The engine checks each: an unknown key code, a
  role its way of reading doesn't have, a table that contradicts itself or a missing `id` is a
  `problem`, and the layout isn't loaded (a stock one falls back to the shipped file).

**Ranking between layouts is trained on the stock layouts** (owner, 2026-11-24). The key models
that decide which layout a run of keys was meant for (bada ime's spec §7.0) are built ahead of time,
one per stock layout, and stored under its string id. They can't follow an edit:

- **A stock layout that was changed** keeps its id, so its model is still used, and the model still
  believes the stock keys. In a profile with other layouts, the wrong layout's reading may show.
- **A layout with a new id** has no model. Among other layouts it is ranked by the word tables
  alone, as v1's layouts 19, 150 and 151 are.
- A layout used alone in a profile is unaffected either way: there is nothing to choose between.

So **the settings page shows a warning before it saves a change to a layout's JSON**, saying which
of the two applies, and what it costs. It is shown where the JSON is edited, not after. A changed
stock layout carries a "modified" mark in every list from then on, and the engine reports it
(`P_describe.layouts[].stock`, `modified`). The CLI does the same in its own way (§9.2).

### 6.6 More layouts worth adding

Established layouts only, in the same naming. The Korean 3-set rows were computed from libhangul's
files, as `ko-3-set-unshifted` was before it was left out (§6.2): a stock file for any of them
needs a source that an MIT repository can take.

| Id | What | On JIS |
|---|---|---|
| `en-colemak-dh` | Colemak-DH, the common newer Colemak | Letters only: nothing lost |
| `en-workman` | Workman | Letters only |
| `en-norman` | Norman | Letters only |
| `en-dvorak-left`, `en-dvorak-right` | One-handed Dvorak | They move digits and symbols: to be checked |
| `en-dvorak-programmer` | Programmer Dvorak | Rearranges the whole number row: at odds with the JIS rule |
| `ko-3-set-unshifted` | 세벌식 순아래 (libhangul `3s`) | Can't type **`:` `@` `^`**; ⇧R, ⇧A and ⇧V were to take them |
| `ko-3-set-390` | 세벌식 390 (libhangul `39`) | Can't type **`:`** only; ⇧0 is free |
| `ko-3-set-final` | 세벌식 최종 (libhangul `3f`) | Can't type 12: `# $ & * + @ [ ] ^ { }` and `` ` `` |
| `ko-3-set-dubeol` | 세벌식 두벌배열 (libhangul `32`) | Nothing lost |
| `ko-2-set-archaic`, `ko-3-set-archaic` | 옛한글 (libhangul `2y`, `3y`) | Not checked |
| `ko-romaja` | Korean typed in Roman letters (libhangul `ro`) | Letters only |
| `ko-ahnmatae` | 안마태 (libhangul `ahn`) | Not checked |
| `ko-3-set-shin` | 신세벌식 P2 (community; not in libhangul) | Not checked |
| `ja-azik` | AZIK, extended romaji | Uses `;` `:` `@` as letters: to be checked |
| `ja-kana-new-jis` | 新JIS (JIS X 6004) | Kana on three rows; digits stay typable |
| `ja-tsuki` | 月配列 2-263 | Prefix keys instead of ⇧ |
| `ja-nicola` | 親指シフト | Needs 無変換 and 変換 as shift keys, which are the harness's language keys (§8), and key-up |

## 7. Henkan profiles: n of them

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

The file has three parts: `engine` (this section; the only part the harness sends to the engine),
`keys` (the harness's key table, §8) and `harness` (one harness's own settings, by its id).

- **`id`**: `[a-z0-9-]{1,32}`, unique, chosen by the user, never the name of a key. The key table,
  the state and `select` refer to a profile by it. **`name`** is what menus show.
- **No fixed count, no reserved names.** None is allowed, and is how a new install starts (§7.1). The
  format has no upper limit; an engine must take at least 32 and must refuse more than it takes
  with an error, never truncate.
- **No links.** A profile the user doesn't want doesn't exist. What profiles share is in
  `henkan.defaults`, and a profile overrides it field by field: `weights`, `script_forms` (and its
  method), per-layout `options`.
- **`layouts`**: the candidates, by layout id, built-in or custom, in order (earlier wins a tie). The
  `en-*` layouts may be candidates, as QWERTY is today.
- **`muhenkan.layout`** is now a real choice: `en-qwerty`, `en-dvorak`, `en-colemak` or a custom one.
  Unset, as on a new install, the engine passes every key on and the OS types it.
- **`fkeys`** are **per profile** (owner, 2026-11-24): each profile has its own F6–F10 (`intercept`,
  and each key `swallow`, `pass` or `<layout>.<action>`), with nothing shared between profiles. A
  profile without `fkeys` intercepts them and binds none, as v1's default. They stay the engine's:
  the harness feeds it the F keys.
- `select` with another profile commits what's staged, as v1. The profile already in use: nothing.
- **An unknown id is an error** (`unknown-profile`, `unknown-layout`), in `select` and in the settings alike.
- **Where the user sees it.** The OS still gets exactly two input sources (bada spec §4.1 stands: 英数
  and かな route to them). Profiles are inside henkan: the harness's input menu lists
  `henkan.profiles` by `name`, and replies echo `profile`, for an indicator. A harness that keeps
  the mode across processes (TSF) stores the id string, not an index.

### 7.1 A new install: no profiles, and the onboarding

The first settings file has **no muhenkan layout and zero henkan profiles** (owner, 2026-11-24).

- With no profile, a key that would enter henkan (かな, 変換, a toggle) doesn't: the harness opens the
  settings window on its **onboarding**. The engine is not asked; `select` with henkan and no
  profile would answer `no-profile`.
- **Easy mode**: pick the muhenkan layout and one henkan layout (`en-qwerty` and `ja-romaji`, say).
  That writes `muhenkan.layout` and one profile with that one layout, and the key that was pressed
  works from then on.
- **Skip**: the full settings, right away.
- The same from the command line, for an agent: `bada settings set engine.muhenkan.layout en-qwerty`
  and `bada profile add main --layouts ja-romaji`.

**Nothing is carried over from bada ime 0.3.1** (owner, 2026-11-24): bada is a new input method
beside it (§12), it starts fresh, and it never reads the old one's settings.

## 8. Keys the harness decides

The engine never sees a language key or Caps Lock. The harness reads them, decides, and feeds the
engine the result: a `select`, an `action`, or nothing.

### 8.1 The key table

The `keys` part of the settings file. It is the harness's, in one format for every harness, with
keys as W3C codes so a file moves between platforms:

```json
"keys": {
  "bindings": [
    {"on": "CapsLock",    "do": ["mode.toggle"]},
    {"on": "Lang2",       "do": ["mode:muhenkan"]},
    {"on": "Lang1",       "do": ["profile:main", "menu.next"]},
    {"on": "shift+Lang1", "do": ["menu.prev"], "when": "henkan"},
    {"on": "ctrl+Lang1",  "do": ["fix"], "when": "henkan"},
    {"on": "Convert",     "do": ["profile:work", "menu.next"]},
    {"on": "KanaMode",    "do": ["profile:ja", "menu.next"]},
    {"on": "Lang1",       "do": ["menu.next", "profile:main"], "when": "profile:ja"},
    {"on": "ctrl+alt+shift+meta+Space", "do": ["profile.next"]}
  ],
  "cycle": ["main", "work", "ja"],
  "henkan_enters": "last",
  "caps_lock": "respect",
  "caps_rule": "allcaps"
}
```

- **`on`**: one key, with or without modifiers, as recorded from a real press.
- **`do`**: a list, and **the first action that applies runs**. An action doesn't apply when it would
  change nothing: `profile:x` when x is in use in henkan mode, `mode:m` in m, `menu.next` when the
  last reply's menu has one row or none. If none applies the key is swallowed, unless the list ends
  in `pass`. The harness knows all it needs: its own mode and profile, and the last reply.
- **`when`**: `muhenkan`, `henkan`, or `profile:<id>`. The most specific binding for a key wins
  (profile, then mode, then none); among equals, the later one.
- **`cycle`**: the profiles `profile.next` / `profile.prev` go through, in order.
  **`henkan_enters`**: the profile that `mode:henkan` and the toggle enter: `"last"` or an id.

| Action | The harness then |
|---|---|
| **`mode.toggle`** | Goes to the other mode, as Caps Lock does today on its "toggle" setting: `select` with muhenkan, or with henkan and `henkan_enters`' profile |
| `mode:muhenkan`, `mode:henkan` | Switches the OS input source as it does now, and sends `select` |
| `profile:<id>`, `profile.next`, `profile.prev`, `profile.last` | Enters henkan if needed, and sends `select` with the profile |
| `menu.next`, `menu.prev`, `fix`, `<layout>.<action>` | Sends `action` |
| `pass`, `none` | Lets the app have the key; swallows it |

### 8.2 The toggle, and the keys it can go on

**`mode.toggle` is an official action**, and like every action it can be bound to any language key
the harness detects:

| Key | Code | Windows | Linux | macOS |
|---|---|---|---|---|
| 半角/全角 (H/Z; `tilde` on a Mac) | `Backquote` | yes | yes | yes: a PC JIS board's key arrives as the `` ` `` key. Apple's JIS boards don't have it |
| 無変換 (Muhenkan; `pc_muhenkan`) | `NonConvert` | yes | yes | Read at the HID level: needs Input Monitoring |
| 変換 (Henkan; `pc_henkan`) | `Convert` | yes | yes | The same: macOS makes no key event of it |
| カタカナ/ひらがな (Kana; `pc_katakana`) | `KanaMode` | yes | yes | The same: macOS turns it into かな |
| 英数 (`eisuu`) | `Lang2` | | | yes; everywhere once Accessibility is allowed |
| かな | `Lang1` | | | yes; everywhere once Accessibility is allowed |
| Caps Lock | `CapsLock` | yes | yes, but an input method can't set the lock, so its light follows the key | Needs Input Monitoring and Accessibility; without them it's macOS's |

**Two language keys together are a binding too** (owner, 2026-11-24): `KanaMode+Lang1` (かな with
カタカナ), `Convert+Lang1`, any two of the keys above but Caps Lock, held together and released with
no other key in between. A pair can be bound like any key (to a profile, say) wherever the harness
sees both keys; no default binds one. A key that is part of a bound pair runs its own binding when
it is released alone, or when another key is pressed, instead of when it is pressed.

(The macOS column is bada ime's spec §6.1, §6.6 and §6.7 as built.) Bound to anything, Caps Lock
stops being Caps Lock: `caps_lock` (`toggle` is now just the binding above; `transient`, `respect`)
and `caps_rule` say what the lock itself does, as v1. A key bound here isn't fed to the engine, so
半角/全角 as a toggle gives up full-width / half-width on that key, which is its default (§8.4).

### 8.3 Any other key, once the OS allows it

**With the permissions it needs, the harness takes any chord as a binding**: `hyper+Space`
(`ctrl+alt+shift+meta+Space`), a lone modifier tap, an F key.

- **macOS**: Accessibility (the key tap), and Input Monitoring for keys read at the HID level.
- **Windows, Linux**: the text service is handed the focused app's keys, modifiers included, so
  chords need no permission; a chord the desktop takes first (Super+Space in GNOME) never arrives.
- The harness publishes what it can detect now, in the state: `harness.keys.available` (the language
  keys of the table above that it sees) and `harness.keys.chords` (true or false, with the missing
  permission's name). The settings page and the CLI read it: they offer chord bindings only when it
  is true, and list a binding the harness can't see as inactive instead of dropping it.

### 8.4 Each harness reads the table itself

There is no shared code for this (owner, 2026-11-24): every harness, and the CLI, carries out §8.1
to §8.3 in its own language, as the Mac's `ProfileKeyLogic` does today. What they share is the
table's format and its rules, which are the standard's, so one settings file means the same on
every platform.

To keep the copies from drifting as v1's did (§2, A), the standard also carries **cases**: a key
press, the harness's state (mode, profile, menu rows, Caps Lock) and the table, with what must
happen. Each harness's own tests replay them, as `mac/tools/test_profile_keys.swift` checks the
Mac's today. They are test data, not code (owner, 2026-11-24).

**Defaults** come with the keyboard, so nothing in the standard is "home":

| Keyboard | Preset |
|---|---|
| JIS on a Mac | 英数 muhenkan; かな the first profile (none yet: the onboarding, §7.1), then next entry; ⇧かな previous; ⌃かな fix. v1's behaviour, key for key |
| JIS on a PC | 無変換 muhenkan; 変換 the first profile, then next entry; カタカナ unbound |
| No language keys | Caps Lock toggle |

**半角/全角 makes full-width, and ⇧半角/全角 half-width, by default on every keyboard that has the key**
(owner, 2026-11-24), as in bada ime 0.3.1: the staged text, or else the selection. They are two
lines of every default table, `{"on": "Backquote", "do": ["width.full"]}` and
`{"on": "shift+Backquote", "do": ["width.half"]}`, so the user sees them and can rebind the key (to
the toggle, say).

**F keys** are not in this table: they are each profile's own, in the engine's settings (§7).

## 9. Settings, files and the CLI

### 9.1 One folder of files

Settings stop living in macOS's user defaults and the Windows registry. Every platform has the same
folder (`~/Library/Application Support/bada`, `%APPDATA%\bada`, `~/.config/bada`):

| File | What |
|---|---|
| `settings.json` | `bada-settings/2`: `engine`, `keys`, `harness` (§7, §8) |
| `layouts/*.json` | The user's layouts: new ones, and changed copies of stock ones, which replace them (§6.5) |
| `dictionary.json` | The user dictionary, as v1 |
| `history/` | The last few `settings.json`, for undo |

The running input method watches the folder and reloads; the settings page and the CLI write files
(a temporary file, then a rename) and never talk to it directly. So an input method that isn't
running can still be set up, and nothing needs a socket.

- The harness sends the engine the `engine` part: `settings` with `set` replaces, with `patch`
  merges. The reply has `settings` (in effect) and `problems` (`[{path, code, message}]`). Engines
  whose profile was removed or changed commit what they have staged at their next event.
- **Invalid JSON, a wrong `format`, or an error-level problem changes nothing**: the last good
  settings stay in effect, and the problems are in the state for the settings page to show.
- `P_describe`'s `settings.schema` is a JSON Schema for the `engine` part, with titles in en / ko /
  ja; the standard has the schema for `keys`. **One settings page**, driven by the two, serves every
  harness (WKWebView, WebView2, a browser on Linux, the simulator).

### 9.2 `bada`, the command line

A harness with no text field: it links the engine (to check what it writes, with `dry: true`), reads
and writes the folder above, and installs releases. Everything the settings page can do, it can do,
so an AI agent can set bada up for the user.

| Command | Does |
|---|---|
| `bada version` | The CLI's, the engine's and the standard's versions, and the installed input method's |
| `bada update` | Installs the newest release from `team-mirae/bada` on GitHub. `--check` only looks |
| `bada settings show` \| `get <path>` \| `set <path> <json>` \| `validate` \| `import <file>` \| `export` \| `undo` | The whole file, or one value by path (`engine.henkan.profiles[0].layouts`) |
| `bada profile list` \| `add <id> --layouts …` \| `remove <id>` \| `move <id> <n>` \| `set <id> …` | The profile list (§7) |
| `bada keys list` \| `bind <key> <action>…` \| `unbind <key>` \| `available` | The key table (§8). `available` is what the harness detects now |
| `bada layout list` \| `show <id>` \| `add <file>` \| `set <id> <path> <json>` \| `remove <id>` \| `reset <id>` \| `copy <id> <new-id>` \| `validate <file>` \| `missing <id>` | Layouts, stock and the user's. `reset` restores a stock layout. `missing` prints what a layout can't type on JIS and its free slots (§6.4). A change to a stock layout is refused without `--modify-stock`, and the answer says why (the ranking warning of §6.5); with it, and for any new id, the warning is in the output |
| `bada dict show` \| `add <word>` \| `remove <word>` \| `import <file>` | The user dictionary |
| `bada type --profile <id> "<keys>"` | Types on the engine with no text field and prints the result: a way to test a profile or a custom layout |
| `bada state` | The published state, read-only |
| `bada schema [settings\|layout\|keys]` | The JSON Schemas |

- **For agents**: every command takes `--json` and answers on stdout, problems included, with a
  non-zero exit code on error; nothing asks a question. `bada schema` and `bada settings validate`
  let an agent check a change before making it; `bada type` lets it test one; `bada settings undo`
  takes one back.
- **It changes settings, not typing.** The CLI can't switch mode or type into an app: the state
  stays read-only from outside, as v1.
- **`bada update`** replaces a program that sees every key, so: **only when asked**. The input
  method never contacts GitHub by itself, not even to look (owner, 2026-11-24): `bada update` and
  `bada update --check` are the only ways; the download is checked
  against a signature whose key is in the installed build; the Mac app is swapped in place (its
  bundle path must never be absent, and its signing identifier must stay the bundle id: both from
  the JIS Korean troubleshooting notes); the Windows installer, unsigned today, needs signing first.

## 10. Data and dictionary

**Data.**

- Buffers are **named**. The harness passes what it has; `P_describe.data` lists what the engine
  knows. A missing optional buffer turns its feature off; an unknown name is ignored. Adding a
  buffer breaks no harness.
- The engine's own build writes one **pack** (a table of names and blobs). A harness maps the file
  and passes each blob with `P_BORROWED`; blobs the engine can use in place (the kanji lattice, the
  key models, sorted word tables) are then shared between processes by the OS. This is the fix
  pado's docs ask for; how far the word lists go binary is engine work, not contract.
- `key-models.bin` moves into the pack like the rest.

**Dictionary.** A `dictionary` event sets the session's user dictionary; the reply has the summary.
No process-wide state, no caller-freed string.

**Not in 1.0**: a path for the engine to hand learned words back to the harness. JIS Korean had one
(`jk_learner_export`); bada dropped it for the learner app.

## 11. One repository

`team-mirae/bada`, MIT, one Cargo workspace:

| Path | What |
|---|---|
| `docs/` | The standard (bada-compatible v2), the spec, a page per layout |
| `engine/` | The engine (Rust), its data and pack, the eleven layout files |
| `crates/compat` | The contract layer: types, `export!` (which also writes the C header), the session, the panic guard |
| `cli/` | `bada` (§9.2) |
| `harness/mac`, `harness/windows`, `harness/linux`, `harness/web` | The four harnesses: bada ime's Mac shell and simulator, bada-windows, bada-linux |
| `settings/` | The settings page, with the layout editor |
| `tools/` | The conformance runner, the typing robot, the renderer |

- **The standard leads.** Changes go to `docs/` first; the engine uses `crates/compat` like any
  third-party engine would, so the reference can't drift from the standard again.
- **Conformance is a program**: it loads any engine and replays transcripts (JSON Lines: events and
  the replies expected). The same transcripts test each harness's `apply`; the key table has its
  own cases (§8.4).
- **One release** per version, from CI: the input method for each platform, the CLI, the pack.
  `bada update` takes it from there.
- **The existing repositories are not touched**: bada-ime, bada-compatible (v1), bada-windows,
  bada-linux, pado-ime and ara-ime stay as they are, under their names. What bada needs from them
  is copied into the new repository, with its history left behind.
- Third-party data keeps its own license inside an MIT repository (Mozc's dictionary, as now).

## 12. Identifiers: com.teammirae

**The new input method is `bada`, under `com.teammirae`. Nothing that already exists is touched**
(owner, 2026-11-24): bada ime, pado ime, ara ime, bada-windows, bada-linux, JIS Korean and JIS
Moonshot keep their names, their repositories and their ids (`me.sjun.…`, `bada-windows`,
`me.sjun.BadaLinux`). The publisher of the new one is **Mirae Research**, as theirs is.

| What | bada ime 0.3.1 and its ports (stay as they are) | bada (new) |
|---|---|---|
| Engine id (`P_describe`) | none | `com.teammirae.bada` |
| Harness ids | `bada-windows`, `bada-linux` in `host.name` | `com.teammirae.bada.mac`, `.windows`, `.linux`, `.web`, `.cli` |
| macOS bundle | `me.sjun.inputmethod.Bada` | `com.teammirae.inputmethod.Bada` (`inputmethod` must be in the id) |
| macOS modes | `me.sjun.inputmethod.Bada.Muhenkan`, `.Henkan` | `com.teammirae.inputmethod.Bada.Muhenkan`, `.Henkan` |
| macOS connection, keylayouts, login agent | `me.sjun.inputmethod.Bada_Connection`, `.keylayout.*`, `.login` | The same under `com.teammirae.inputmethod.Bada` |
| macOS state notification | `me.sjun.inputmethod.Bada.state` | `com.teammirae.inputmethod.Bada.state` |
| Linux D-Bus | `me.sjun.BadaLinux.State` at `/me/sjun/BadaLinux` | `com.teammirae.Bada.State` at `/com/teammirae/Bada` |
| IBus component | `me.sjun.BadaLinux.<id>` | `com.teammirae.Bada` |
| Windows CLSID and profile GUID | pado's | New ones |
| Windows registry | `Software\bada-windows\<name>` | Registration only (TSF needs it); settings are files (§9.1) |
| Windows version resource | ProductName `bada-windows`, no CompanyName | ProductName `bada`, CompanyName `Mirae Research` |
| Windows state | `Local\bada-windows.state.v1.{CLSID}`, message `bada-windows.state` | `Local\com.teammirae.bada.state.v2.{CLSID}`, message `com.teammirae.bada.state` |
| Files | `Application Support/me.sjun.inputmethod.Bada`, `Application Support/bada-ime`, `%APPDATA%\bada-windows`, `~/.config/bada-linux` | A folder named `bada` (§9.1) |
| Layouts | Integers, with ranges reserved by Mirae Research | Slugs (§6.1) |

Because every id, folder and GUID is new, **the two can be installed side by side** and neither
reads or writes the other's files. On a Mac (from the JIS Korean and Moonshot troubleshooting notes):

- bada is a new input method to macOS: the user adds its two sources, and they may not appear until
  logout.
- Accessibility and Input Monitoring are granted per bundle id: bada asks for its own.
- The code-signing identifier must equal the bundle id, or `imklaunchagent` refuses the connection
  and nothing types.
- With both installed, only one should have its key tap running.

## 13. Order of work

1. Settle §14. Create `team-mirae/bada`, a new repository; write the v2 standard and the schemas in `docs/`.
2. `crates/compat` (types, `export!`, session, panic guard), the key table's cases, the conformance runner
   and its first transcripts (from bada-compatible's `tests/conformance.rs`, `shout`, and
   `mac/tools/test_profile_keys.swift`).
3. The engine, copied from bada-ime and cut down to the eleven layouts. Every layout's rules and
   tables move out of the code into its file, read at run time by general machinery (§6.5): slug ids, profiles as a list,
   `select` / `action` / `layout`, the JIS table and `types`, named data and the pack. `en-dvorak`,
   `en-colemak` are new files, as §6.4 has them. Key models
   rebuilt. The typing robot runs throughout.
4. `cli/`: `version`, `settings`, `profile`, `keys`, `layout`, `type` first; `update` once releases exist.
5. `harness/mac` with the new bundle id: `ProfileKeys.swift` and the Caps Lock rules are rewritten
   for the key table (n profiles, the toggle, chords); the key tap, HID reading and input-source
   switching stay as they are.
6. `settings/`, from the schemas. Then `harness/windows`, `harness/linux`, `harness/web`.
7. The first release; `bada update`.

## 14. Decisions

Settled by the owner, 2026-11-24:

| | |
|---|---|
| Who decides what language keys and Caps Lock do | The harness. The engine only sees what its harness feeds it |
| Name and home | **bada**; one repository, `team-mirae/bada`, MIT, with the engine and every harness |
| Ids | The new input method is `bada` under `com.teammirae`, publisher Mirae Research. (An earlier `com.themrsung` was withdrawn.) |
| What already exists | Not touched: bada ime, pado, ara, bada-windows, bada-linux, bada-compatible v1, JIS Korean and JIS Moonshot keep their names, repositories and ids |
| ABI | One `P_send` with event types, and three objects: data, session, engine (§4) |
| Shortcuts | ⌘ and ⌃ follow the QWERTY key positions under every layout (§6.3) |
| Updates | Only by `bada update` (or `--check`); the input method never checks by itself |
| bada ime 0.3.1's settings | Not read, not migrated: bada starts fresh |
| Layout files (§6.5) | Everything a layout is lives in its JSON; the user can change any of it, the stock layouts included |
| Ranking models | Trained on the stock layouts and stored under their ids; the settings page warns before saving a change to a layout's JSON |
| 半角/全角 | Full-width, and ⇧ half-width, by default |
| Layouts | Slug ids, no integers. The eleven of §6.2. Of Mirae Research's own layouts only bada roman is kept, as `ko-bada-roman`; `en-qwerty-us` has the US symbols, with `` ` ~ `` on ろ |
| Symbols | The JIS legends on every platform, unless the layout overrides; the OS layout is ignored |
| Custom layouts | JSON files, through the settings page, the C ABI or the CLI |
| CLI | `bada`: `update` from GitHub releases, `version`, and every settings and profile operation |
| Toggle | `mode.toggle`, bindable to any language key the harness detects; any chord once the OS allows it |
| `ja-kana` | Keeps the ⌥⇧ layer (Alt+Shift off the Mac) |
| First run | No default layout and zero henkan profiles; かな or 変換 opens the settings window's onboarding (easy mode, or skip to the full settings) |
| F keys | Per profile |
| The key table's code | Each harness's own; no shared crate. The format, the rules and a set of test cases are the standard's, and each harness's tests replay the cases |
| The layouts on JIS (§6.4) | Dvorak and Colemak keep their own symbol places; the characters that lose their key move: Dvorak `@` `` ` `` to the `]` key, `~` ⇧0, `^` ⇧2, `*` ⇧7; Colemak `+` ⇧0. `ja-kana` as it is now |
| `ko-3-set-unshifted` | Left out of the stock layouts: its key map came from an LGPL-2.1 file |
| Pairs of language keys | Two language keys held together (`KanaMode+Lang1`, `Convert+Lang1`, …) are a binding of their own, for any action; unbound by default (§8.2) |
| ¥ in the Korean layouts | `ko-2-set` and `ko-bada-roman` type ₩ on the ¥ key (owner, 2026-11-24); ⇧¥ stays `\|` |

Open: nothing. Every item this proposal raised is settled above.
