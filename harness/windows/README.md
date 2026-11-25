# harness/windows

bada's Windows input method: one TSF text input processor (`bada-tsf.dll`) around the engine in
`engine/`. The harness gets keys from the Text Services Framework, feeds the engine, puts the text
in the app as a composition, and draws the menu. The interface between the two is the standard,
[docs/bada-compatible-v2.md](../../docs/bada-compatible-v2.md); section numbers below are its.

It is a port of bada-windows (bada ime's Windows port, for the v1 ABI) to v2: the TSF text service,
the composition handling, the candidate window, the key event sink, the compartments and the
registration are kept; the engine ABI, the key table, the settings and the state are all new. The
two install side by side: no id, GUID, registry key, file or folder is shared (§12).

| What | bada |
|---|---|
| Harness id | `com.teammirae.bada.windows` |
| TIP CLSID | `{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}` |
| Language profile GUID | `{89CC34D8-9E73-44BD-B1F9-301D19A5C30F}` (listed under Japanese, `ja-JP`) |
| Registry | registration only (§12): `HKLM\SOFTWARE\Classes\CLSID\{CLSID}` and `HKLM\SOFTWARE\Microsoft\CTF\TIP\{CLSID}`. Nothing else; the settings are files |
| Settings folder | `%APPDATA%\bada` (§10.1) |
| State | `Local\com.teammirae.bada.state.v2.{CLSID}`, window message `com.teammirae.bada.state` (§6.7) |
| Version resource | ProductName `bada`, CompanyName `Mirae Research`, from the repo's `VERSION` (0.1.0) |

**Tested** on Windows 11 Pro (x64), MSVC 14.44, in the machine's interactive session: the headless
tests, and the text service end to end in a TSF thread of the test's own (below). **Not tested**:
on a real keyboard in real apps (the steps are in "Verify by hand"); ARM64 (the code doesn't block
it — add an `aarch64-pc-windows-msvc` target and a preset).

## Layout

| Path | What |
|---|---|
| `src/core/` | C++20 with no Windows headers: `keytable` (§9), `keys` (Windows messages → key codes, §4), `engine` (the C ABI, §3), `reply` (replies, and how one is applied: §6.3), `settings` (the folder: §10.1), `harness` (the session and one context per text field), `state_block` (the published state's shared memory: §6.7), `json`. Tested on any OS |
| `src/tip/` | The TSF text service, `bada-tsf.dll`: the text input processor, key event sink, edit sessions, the composition, the candidate window, the taskbar indicator, the state and its shared memory, registration |
| `tests/` | The key-table cases, the conformance transcripts, the core, the state, and the text service end to end (`tsf_host`, on a TSF text store of its own) |
| `tools/` | `build.ps1`, `test.ps1`, `register.ps1`, `send.sh`, `make-icon.py` |

The engine is linked in (`bada.lib`, the static CRT), so the DLL is one file. **Its data is a folder
of files** (§11), `data` beside the DLL's own folder (`%ProgramFiles%\bada\data`, or `build\data`
for a build): each is the buffer of its name (`kanji.bin` is `kanji`), mapped and passed by
`EngineData::Load` in `src/core/engine.cpp`. The ones the engine uses in place stay mapped, so
Windows shares them between the apps; the word lists are read once in each app, when the text
service starts there. A file that is missing turns its feature off: with none, every layout still
types, without kanji conversion or word ranking. `BADA_DATA_DIR` names another folder. Once the
pack's layout is in the standard, the pack is mapped instead.

## Build

Needs Visual Studio Build Tools 2022 (MSVC x64, and x86 for 32-bit apps), CMake, Ninja, and Rust
(rustup) with the matching target. `tools/build.ps1` finds Visual Studio, puts cargo on PATH, and
builds the engine (static CRT) and the harness. Warnings are errors.

```powershell
tools\build.ps1              # x64, into build\x64\
tools\build.ps1 -Arch x86    # for 32-bit apps (needs: rustup target add i686-pc-windows-msvc)
```

CMake runs `cargo build --release -p bada-engine` itself, into the build folder (the repository's
own `target` is left alone); `-DBADA_ENGINE_LIB=<bada.lib>` uses one built elsewhere. The version is
the repository's `VERSION`.

On a Mac, the portable core and its tests build and run with CMake and Ninja too (no TSF):
`cmake -S harness/windows -B build/host -G Ninja && cmake --build build/host`.

## Test

```powershell
tools\test.ps1               # all of the below; the text service from its DLL in a TSF thread
tools\test.ps1 -InSession    # from a logon that isn't interactive (SSH): see the note below
tools\test.ps1 -Registered   # the text service as the registered profile (tools\register.ps1 first)
```

| Test | What |
|---|---|
| `bada_keytable_cases` | Replays **every case and every sequence** of `docs/cases/key-table.json` on `src/core/keytable` (§9.6), and checks that the presets are the standard's |
| `bada_transcript` | Replays `tools/conformance/transcripts/any-engine.jsonl` and `bada.jsonl` on the linked engine, through the engine wrapper, applying every reply to a plain text field as §6.3 says |
| `bada_core_tests` | The core, typed into the linked engine: Windows' keys, the settings folder, the key flow, `types`, Caps Lock, two fields and a password field, the mode shared between processes, and the published state with its shared-memory block |
| `bada_state_tests` | The state's shared memory, written and read as the text service and a reader do; the window message |
| `bada_tsf_host` | **The text service end to end**, with no desktop: a TSF thread manager and a document on an `ITextStoreACP` of the test's own, the service activated on it, every key fed in through its key event sink, the text read back out of the store. Covers QWERTY muhenkan, the JIS legends, Caps Lock, Korean 2-set (`gks` → 한, Space commits), romaji (`ka` → か), a mode switch and a profile switch through the key table, the menu and its display attributes, 半角/全角 reading the selection, a pair of language keys, a settings error in the state, and the mode shared across two processes |

**TSF works only in an interactive logon.** Over SSH (a service logon, session 0) the thread manager
can't be created, so `bada_tsf_host` reports that and `tools\test.ps1 -InSession` runs it in the
session of the user logged on at the machine, through a scheduled task with an interactive logon type
(no password), on a desktop of the test's own that is never shown. Nothing appears on the user's
screen and nothing is typed into their apps. Run at the machine's own console, plain `tools\test.ps1`
runs it directly (it is already interactive).

## Register, and try it

Registering writes the two registry keys above and nothing else. It adds bada to **nobody's**
keyboards, changes no language and makes nothing the default: the user adds the keyboard themselves.

```powershell
tools\register.ps1               # regsvr32 build\x64\bada-tsf.dll (asks to elevate; UAC)
tools\register.ps1 -Arch x86     # the 32-bit DLL, for 32-bit apps (register both for both)
tools\register.ps1 -Status       # what is registered now
tools\register.ps1 -Unregister   # regsvr32 /u, and nothing of bada is left in the registry
```

Then, in **Settings → Time & language → Language & region → 日本語 → … → Keyboards → Add a
keyboard**, pick **bada**, and switch to it with Win+Space. **Restart apps that were running** before
you registered: they keep the old DLL, or none.

## The installer

`tools\package.ps1` builds `build\dist\bada-<version>-setup.exe` with
[Inno Setup 6](https://jrsoftware.org/isinfo.php) (`installer\bada.iss`): what a release carries.

```powershell
tools\package.ps1              # build x64 and x86, then package
tools\package.ps1 -SkipBuild   # package what is in build\ already
```

It copies both DLLs and the engine's data to `%ProgramFiles%\bada`, registers the DLLs as
`tools\register.ps1` does, and adds bada to the Japanese keyboards of the user who ran it
(`installer\keyboards.ps1`). Apps that are open are left open: a DLL or a data file in use is moved
aside, and those apps load the new one when they restart. Uninstalling removes bada from the keyboards and the registry, and asks whether to delete
`%APPDATA%\bada` too. The installer isn't signed unless `BADA_SIGN_CERT` names a certificate, so
Windows warns about it.

## First run

A new install has **no muhenkan layout and no henkan profile** (§8.3). Keys type as the system's
layout has them, and a key that would enter henkan (変換, or whatever the key table binds) doesn't:
bada shows a short notice and puts it in the state (`harness.notice`). The shared settings page will
open there once it exists. Until then, write `%APPDATA%\bada\settings.json` by hand. The easy-mode
minimum is a muhenkan layout and one profile:

```json
{"format": "bada-settings/2",
 "engine": {"muhenkan": {"layout": "en-qwerty"},
            "henkan": {"profiles": [{"id": "main", "name": "日本語", "layouts": ["ja-romaji"]}]}}}
```

The folder is watched (re-read on the next key), so 変換 works from then on. The layouts are in
[engine/layouts/](../../engine/layouts/) (`ko-2-set`, `ja-kana`, `ja-romaji`, `en-dvorak`, …); a
profile may have several. A fuller file, with the key table and this harness's own settings:

```json
{"format": "bada-settings/2",
 "engine": {
   "muhenkan": {"layout": "en-qwerty"},
   "henkan": {"profiles": [
     {"id": "ko", "name": "한국어", "layouts": ["ko-2-set"]},
     {"id": "ja", "name": "日本語", "layouts": ["ja-romaji"]}]}},
 "keys": {
   "bindings": [
     {"on": "NonConvert", "do": ["mode:muhenkan"]},
     {"on": "Convert",    "do": ["profile.first"]},
     {"on": "Convert",    "do": ["menu.next", "profile.first"], "when": "henkan"},
     {"on": "shift+Convert", "do": ["menu.prev"], "when": "henkan"}],
   "cycle": ["ko", "ja"]},
 "harness": {"com.teammirae.bada.windows": {"keyboard": "auto"}}}
```

## Settings

`%APPDATA%\bada` is the standard's folder (§10.1). The harness reads it at the start, when a field
gains focus, and a few times a second while keys are pressed (so a file written while typing is
taken up). Nothing is ever written here by the harness, and nothing is read from the registry.

| File | Goes |
|---|---|
| `settings.json`, `engine` | To the engine, with a `settings` event (§8) |
| `settings.json`, `keys` | The key table (§9), read by `src/core/keytable` |
| `settings.json`, `harness["com.teammirae.bada.windows"]` | This harness's own settings (below) |
| `layouts\*.json` | To the engine, one `layout` event each, before the settings (§7.6) |
| `dictionary.json` | To the engine, with a `dictionary` event (§11) |

**A file that can't be used changes nothing.** Invalid JSON, another `format`, a key table with an
error, or settings the engine refuses: what was in effect stays, all of it, and the problems are
published in the state (`harness.problems`, each with its `file`). `settings.json` is one unit: an
error in any of its three parts, and none of it is taken.

The harness's own settings, under `harness["com.teammirae.bada.windows"]`:

| Field | Default | |
|---|---|---|
| `keyboard` | `"auto"` | Which preset a file with no `keys.bindings` gets (§9.5): `auto` (`jis-pc` when Windows reports a Japanese keyboard, `none` otherwise), `jis-pc` (無変換, 変換), `jis-mac` (an Apple JIS keyboard's 英数, かな) or `none` (Caps Lock toggles) |
| `publish_state` | `true` | Publish the state for other programs (§6.7) |
| `log` | `false` | Write `%LOCALAPPDATA%\bada\bada.log`: activations, focus and errors, never typed text |

## Keys

- **Keys go by position**: the scan code, whatever keyboard layout Windows has loaded under the
  Japanese profile. 無変換 is `NonConvert`, 変換 `Convert`, カタカナ/ひらがな `KanaMode`, 半角/全角
  `Backquote`; an Apple JIS keyboard's かな and 英数 arrive as `Lang1` and `Lang2`. The virtual key
  is read only for a key injected without a scan code.
- **The key table decides a press first** (§9). A press it has a binding for isn't fed to the engine;
  a language key or Caps Lock with none goes to Windows as it is; any other key goes to the engine as
  a `key` event.
- **Caps Lock is the harness's** (§9.4). Windows flips the lock itself on the key's press, so where
  the harness decides otherwise (the key is bound, or `transient` keeps it off in henkan) it sets the
  lock back with one injected press, which the key sink lets through.
- **The engine passes a key on** with `types` (§6.4): a typing key types its JIS legend whatever
  Windows' layout has, and the harness inserts the text itself unless Windows' layout would type the
  same. Under the Japanese layout Windows types the JIS legends itself, so those keys reach the app.

## The state, for other programs

bada publishes the engine's state and its own (§6.7) in a block of shared memory,
`Local\com.teammirae.bada.state.v2.{CLSID}`, for any program of the session to read with the
system's own calls — no library, no plugin. It is readable only by the user's own processes at
medium integrity and above (it holds what is being typed), so a sandboxed program (a Store app, a
browser's renderer) can't read it, and the text service inside one can't publish there. The block's
layout is in [src/core/state_block.h](src/core/state_block.h). A registered window message
`com.teammirae.bada.state` is posted (to `HWND_BROADCAST`) when the mode, the focus, a password
field, whether something is staged, or the lock changes; for the text, read the block.
`bada_state.exe` (built with the tests) prints it:

```powershell
build\x64\bada_state.exe            # the state now
build\x64\bada_state.exe --watch    # again at every message, until Ctrl+C
```

**Nothing typed is published in a password field**: `staged`, `preedit` and `menu` are empty and
`harness.secure` is true.

## Verify by hand

The headless tests cover the engine and the key path in full, and `bada_tsf_host` drives the real
text service through a TSF text store. A few behaviours need a real edit control and a real keyboard
and should be checked on a desktop once, with the IME registered and added (above):

1. Type in Notepad, in WordPad (a RichEdit through the IMM layer) and in a browser's address bar: a
   Korean 2-set profile (`gks` → 한, Space commits), romaji (`ka` → か), the menu and its underline.
2. 無変換 / 変換 switch mode; the taskbar indicator shows `A` / the profile; a right click on it opens
   the menu.
3. A password field (a website's sign-in, a Windows credential prompt): nothing composes, and
   `bada_state.exe --watch` shows `"secure":true` with nothing typed.
4. A click elsewhere in the text, and an app that ends the composition, commit what is staged.
5. The JIS language keys by hand on a PC JIS keyboard (injected keys can't stand in for the physical
   ones): 無変換, 変換, カタカナ/ひらがな, and 半角/全角 for full-width / half-width.

## Sending from another machine

The harness can be written on another machine (a Mac, say) and built on Windows, which then has a
copy of the repository. `tools/send.sh` copies the harness over SSH (and `--all` the engine, docs
and tools); then build and test on Windows.

```sh
BADA_WINDOWS_HOST=user@host harness/windows/tools/send.sh          # harness/windows
BADA_WINDOWS_HOST=user@host harness/windows/tools/send.sh --all    # also engine, crates, docs, tools, Cargo.*, VERSION
```

`BADA_WINDOWS_SSH` is the ssh command (default `ssh`), and `BADA_WINDOWS_DIR` the copy's folder
there (default `%USERPROFILE%\Desktop\bada`).

## License

[MIT](../../LICENSE), © 2026 Mirae Research. The TSF text service, the candidate window, the edit
sessions, the display attributes and the indicator are adapted from bada-windows, which is
jis-moonshot's measured TSF code (both CC0); see each file's header.
