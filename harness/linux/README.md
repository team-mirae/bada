# harness/linux

bada's Linux input method: a Fcitx5 addon and an IBus engine around the engine in `engine/`. The
harness gets keys from Fcitx5 or IBus, feeds the engine, puts the text in the app and shows the
menu. The interface between the two is the standard, [docs/bada-compatible-v2.md](../../docs/bada-compatible-v2.md);
section numbers below are its.

It is a port of bada-linux (bada ime's Linux port, for the v1 ABI) to v2. The two install side by
side: no id, file or folder is shared (§12).

| What | bada |
|---|---|
| Harness id | `com.teammirae.bada.linux` |
| Fcitx5 addon, and its input method | `bada`, `bada` |
| IBus component, and its engine | `com.teammirae.Bada`, `bada` |
| Settings folder | `~/.config/bada` (`$XDG_CONFIG_HOME/bada`) |
| State, D-Bus | `com.teammirae.Bada.State` at `/com/teammirae/Bada` |
| State, file | `$XDG_RUNTIME_DIR/bada/state.json` |

**Tested**: on Arch Linux (x86_64), Fcitx5 5.1.23, with no desktop: the tests below drive a real
Fcitx5, in the test's process and as a daemon over D-Bus. **Not tested**: on a desktop, with a real
keyboard and real apps; and the IBus engine, which builds (libibus 1.5.34) but has never been run.

## Layout

| Path | What |
|---|---|
| `src/core/` | C++20 with no Fcitx5 or IBus types: `keytable` (§9), `keys` (evdev codes → key codes), `engine` (the C ABI), `reply` (replies, and how one is applied: §6.3), `settings` (the folder, and its watch: §10.1), `harness` (the session, and one context per text field), `json` |
| `src/common/` | The published state: a D-Bus signal and a file (§6.7) |
| `src/fcitx5/` | The Fcitx5 addon, `libbada-fcitx5.so` |
| `src/ibus/` | The IBus engine, `ibus-engine-bada` |
| `tests/` | The tests (below) |
| `tools/test.sh` | Builds and runs all of them. `tools/e2e-dbus.sh` is its last step, also for an installed copy |

The engine is linked in (`libbada.a`), so the addon is one file. **No data files are passed to it
yet**: every layout types, but there is no kanji conversion and no word ranking. The place where
the pack's blobs will be passed as named buffers (§11) is `EngineData::Load` in
`src/core/engine.cpp`.

## Build

It needs a C++20 compiler, CMake, pkg-config, Rust (for the engine), `dbus-1`, and the headers of the
frontends built: Fcitx5 (`Fcitx5Core`, and `Fcitx5Module` for the test), IBus (`ibus-1.0`).

- Arch: `sudo pacman -S --needed base-devel cmake rust fcitx5 dbus libibus` (and `python-dbus
  python-gobject` for the last test).
- Debian, Ubuntu: `build-essential cmake pkg-config cargo libdbus-1-dev fcitx5 fcitx5-modules-dev
  libfcitx5core-dev libfcitx5utils-dev libfcitx5config-dev libibus-1.0-dev`.

```sh
cmake -S harness/linux -B build/linux -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build/linux -j
```

CMake runs `cargo build --release -p bada-engine` itself; `-DBADA_ENGINE_LIB=<libbada.a>` uses one
built elsewhere. `-DBADA_IBUS=OFF` or `-DBADA_FCITX5=OFF` leaves a frontend out. The version is the
repository's `VERSION`.

## Test

```sh
harness/linux/tools/test.sh          # BUILD=<dir> to build somewhere other than harness/linux/build
```

It needs no desktop. Every step has its own session bus, home and runtime folder, so nothing of the
user's is read or written, and a Fcitx5 that is running is left alone.

| Test | What |
|---|---|
| `bada_keytable_cases` | Replays **every case and every sequence** of `docs/cases/key-table.json` on `src/core/keytable` (§9.6), and checks that the presets are the standard's |
| `bada_transcript` | Replays `tools/conformance/transcripts/any-engine.jsonl` and `bada.jsonl` on the linked engine, through the engine wrapper. Every reply is applied to a plain text field as §6.3 says, and the field is checked against it |
| `bada_core_tests` | The core, typing into the engine: the settings folder and its watch, the key flow, `types`, Caps Lock, pairs, `ask`, two fields, a password field, the state and its D-Bus signal. Run again with ASan and UBSan |
| `bada_fcitx5_e2e` | The addon in a Fcitx5 instance inside the test, with Fcitx5's own test frontend: keys in with keysym and keycode, commits, preedit, menu and state out |
| `tests/e2e/fcitx5_dbus.py` | The addon installed into a staging folder and loaded by a fresh `fcitx5`, driven over D-Bus as an app drives it. The state is read from the file and from the signal |

## Install

```sh
sudo cmake --install build/linux
```

`tools/package.sh` builds what a release carries: `build/dist/bada-<version>-linux-<arch>.tar.gz`,
the Fcitx5 addon as it installs under `/usr`, with an `INSTALL.txt`. The IBus engine is left out of
it, since it has not been run.

| File | |
|---|---|
| `<prefix>/lib/fcitx5/libbada-fcitx5.so` | The addon. Loaded when the input method is first used |
| `<prefix>/share/fcitx5/addon/bada.conf`, `…/inputmethod/bada.conf` | The addon, and its one input method |
| `<prefix>/libexec/ibus-engine-bada` | The IBus engine |
| `<prefix>/share/ibus/component/com.teammirae.Bada.xml` | Its component |

**Fcitx5**: restart it (`fcitx5 -r`), and add **bada** to a group with `fcitx5-configtool` (its
language code is `*`, which the tool calls Multilingual). Then take three keys out of Fcitx5's own hotkeys, in Global Options:
`Zenkaku_Hankaku` out of Trigger Input Method, and `Hangul` and `Hangul_Hanja` out of Activate and
Deactivate. Fcitx5 takes its hotkeys before any input method sees them, so with those in, 半角/全角
and an Apple keyboard's 英数 and かな never reach bada.

**IBus**: `ibus restart`, then add bada in the desktop's keyboard settings or `ibus-setup`.

## First run

A new install has no muhenkan layout and no henkan profile (§8.3). Keys type as the system's layout
has them, and a key that would enter henkan (変換, or whatever the key table binds) doesn't: bada
shows a notice in the input panel, writes a line in the log, and puts the notice in the state
(`harness.notice`). The shared settings page will open there once it exists. Until then, write the
two things its easy mode would, which `bada settings set engine.muhenkan.layout '"en-qwerty"'` and
`bada profile add main --layouts ja-romaji` will do from the command line:

```sh
mkdir -p ~/.config/bada
cat > ~/.config/bada/settings.json <<'EOF'
{"format": "bada-settings/2",
 "engine": {"muhenkan": {"layout": "en-qwerty"},
            "henkan": {"profiles": [{"id": "main", "name": "Japanese - Romaji", "layouts": ["ja-romaji"]}]}}}
EOF
```

The folder is watched: the file is taken up when it is written, and 変換 works from then on. The
layouts are in [engine/layouts/](../../engine/layouts/) (`ko-2-set`, `ja-kana`, `en-dvorak`, …); a
profile may have several.

## Settings

`~/.config/bada` is the standard's folder (§10.1). The harness reads it at the start, when a file in
it changes (inotify) and when a field gains focus.

| File | Goes |
|---|---|
| `settings.json`, `engine` | To the engine, with a `settings` event (§8) |
| `settings.json`, `keys` | The key table (§9), read by `src/core/keytable` |
| `settings.json`, `harness["com.teammirae.bada.linux"]` | This harness's own settings (below) |
| `layouts/*.json` | To the engine, one `layout` event each, before the settings (§7.6). A file removed is a layout removed. The settings are sent again whenever a layout changed, so that settings naming a layout that is gone show as a problem |
| `dictionary.json` | To the engine, with a `dictionary` event (§11) |

**A file that can't be used changes nothing.** Invalid JSON, another `format`, a key table with an
error, or settings the engine refuses: what was in effect stays, all of it, and the problems are
published in the state (`harness.problems`, each with its `file`) and written to the log. A profile
that is removed while it is selected leaves henkan: nothing falls back to another profile.

The harness's own settings:

| Field | Default | |
|---|---|---|
| `keyboard` | `"jis-pc"` | The keyboard, which picks the default bindings when the file has no `keys.bindings` (§9.5): `jis-pc` (無変換, 変換), `jis-mac` (英数, かな) or `none` (Caps Lock toggles). Linux can't tell which keyboard is plugged in, so the user says |
| `publish_state` | `true` | Publish the state for other programs |

## Keys

- **Keys go by position**: the evdev code (Fcitx5's keycode − 8, IBus's keycode), whatever XKB
  layout is loaded. 無変換 is `NonConvert` (94), 変換 `Convert` (92), カタカナ/ひらがな `KanaMode` (93),
  半角/全角 `Backquote` (41, or 85), an Apple keyboard's かな and 英数 `Lang1` and `Lang2` (122, 123).
- **The key table** takes a press first (§9). A press it has a binding for isn't fed to the engine;
  a language key or Caps Lock with none goes to the system as it is; any other key goes to the
  engine. A modifier pressed by itself goes nowhere.
- **Pairs** (§9.3) need key releases, which Fcitx5 and IBus both report. A key that waited and
  passes is handed back to the app with a forwarded key.
- **`types`** (§6.4): when the engine doesn't consume a key and says what it types, the app is left
  to type it if its own layout gives the same text (the key's keysym). Otherwise bada commits the
  text and keeps the key. So under a `us` layout the letters are let through and the JIS symbols are
  committed, and under `jp` nearly everything is let through.
- **Caps Lock** (§9.4): the lock is what each key event says it is. An input method can't set the
  lock on Linux, so its light follows the key. `transient` is carried out by ignoring the lock in
  henkan, not by turning it off. Once Caps Lock is bound, the system still flips the lock at each
  press: bada then keeps its own account of the lock (flipped only by an unbound chord such as
  ⇧Caps Lock). Under the `jp` layout the unshifted key is 英数 and flips nothing.

## What shows

| | Fcitx5 | IBus |
|---|---|---|
| Preedit | In the app when it draws one, else in the panel. `live` underlined; `fixed` underlined and bold; `picked` underlined and highlighted. The caret is `preedit.caret` | `live` single underline, `fixed` low, `picked` double |
| Menu | The candidate window, ten rows a page, the page following the selected row. `note` is the row's comment. Script forms are labelled −2 and −1. A `strip` is horizontal, labelled 1 to 9 and 0 | The lookup table; `note` after the text |
| A click on a row | `pick`, then `flush` | The same |
| Indicator | The sub mode: the profile's name in henkan, and its first character as the label; `A` in muhenkan | The `InputMode` property; a click toggles |
| Input menu | muhenkan, and the profiles by name | No |

- **One input method, both modes.** Neither framework routes a language key to an input source, so
  a second one would only add a deactivate and an activate at every switch. Mode and profile are
  the session's: every field follows, with one `select`, when it next has a key or focus.
- **Focus out, and the app's reset.** Fcitx5 commits the marked text an app draws when focus
  leaves, and apps commit or drop their own on a reset; IBus does both for a preedit in commit mode.
  A second commit from the harness would insert the text twice, so there it sends `flush` and drops
  the reply's commit. Marked text shown only in the panel is committed by the harness. Where what
  shows isn't what `flush` would commit, the marked text is flagged so that nobody else commits it.
- **A password field** (Fcitx5: `Password` or `Sensitive`; IBus: purpose password or PIN, hint
  private) stays in muhenkan whatever the mode, gets no text around the caret, and has nothing typed
  in the state. Fcitx5 keeps input methods out of password fields unless Global Options → Behavior
  allows them.
- **Not drawn**: `dir: "rtl"` (the harness doesn't claim `rtl`), and a row's `lang` (the frameworks
  choose the font).

## The state

`bada-compatible-state/2` (§6.7), published after every reply to a field's event, at focus in and
out, and when the settings are read; only when it changed. Reading it changes nothing.

```sh
gdbus monitor --session --object-path /com/teammirae/Bada        # Changed(s json)
dbus-monitor --session "type='signal',interface='com.teammirae.Bada.State'"
jq -r '.mode, .profile' "$XDG_RUNTIME_DIR/bada/state.json"
```

- The signal is `com.teammirae.Bada.State.Changed(s json)` at `/com/teammirae/Bada`, on the session
  bus. No name is owned and no method answers: for the state now, read the file. It is rewritten
  with a rename, mode 0600.
- `harness`: `id`, `version`, `frontend` (`fcitx5`, `ibus`), `app` (the program, from Fcitx5; IBus
  doesn't say), `secure`, `active` (a field has focus), `caps_lock`, `keys` (`available`: all seven;
  `chords`: true; `missing`: none), `problems` and `notice`.
- With `publish_state: false` the file is removed and one `{}` signal sent.

## Not done

- The IBus engine has not been run.
- Nothing has been tried on a desktop: real apps (which give surrounding text, and how each resets),
  the candidate window, the indicator, a JIS keyboard's keys under X11 and Wayland.
- The onboarding is a notice, not a window: the shared settings page doesn't exist yet.
- No data files (§11): the pack's layout isn't in the standard yet.
- `settings.json`'s `history/` is the settings page's and the command line's to write: the harness
  only reads the folder.
- bada-linux's per-program off list (`off_in`) and its packaging are not carried over.
