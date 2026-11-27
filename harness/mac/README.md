# harness/mac

bada's macOS input method: `Bada.app`, bundle `com.teammirae.inputmethod.Bada`. It is a harness of
[bada-compatible v2](../../docs/bada-compatible-v2.md): it gets keys from macOS, asks its key table
(§9), feeds the engine what the table doesn't take, puts the text in the app and draws the menu.
The engine (`../../engine`, `libbada.a`) is linked in.

It is built with the Command Line Tools: a Makefile and `swiftc`, no Xcode.

## Build and test

```sh
make            # build/Bada.app, universal (arm64 + x86_64)
make test       # the three test programs below
make check      # the built app's own check, on an empty settings folder
```

`make` builds the engine for both architectures into `build/cargo` (so nothing outside this folder
is written), then the app. It signs with the first "Apple Development" identity of the keychain, or
ad hoc when there is none; `make SIGN_IDENTITY=-` signs ad hoc. The code-signing identifier must be
the bundle id, or macOS refuses the input method's connection (the standard, §12): the Makefile
checks it.

`make test` installs nothing and reads none of your settings:

| Test | Checks |
|---|---|
| `Tests/KeyTableCases` | Every case and every sequence of [docs/cases/key-table.json](../../docs/cases/key-table.json) on `KeyTable` and `KeySequencer`, and that the three presets are the cases' own tables |
| `Tests/Transcript` | [tools/conformance](../../tools/conformance)'s `bada.jsonl` and `any-engine.jsonl`, through the engine wrapper and the reply applier, on a text field in memory: the reply against `expect`, and the field against §6.3 |
| `Tests/Unit` | The rest of the pure code: the description and the refusal of another `abi.major`, replies, applying one (panic, `ask`), `types`, the settings folder on a real session, the state, the onboarding's file, the folder watcher |

`Bada --check` (what `make check` runs) starts no input method: it prints the engine linked in, the
settings folder as it would be read (`BADA_HOME` moves the folder) and the state as it would be
published.

## Install

**`make` installs nothing.** To install:

```sh
make install      # to ~/Library/Input Methods, registered, with a login agent
make uninstall    # removes the app and the login agent; the settings folder stays
make dist         # build/Bada-<version>.zip
make install-prebuilt PREBUILT=path/Bada.app
```

From a release, with no checkout: `Bada-<version>.zip` is the app. It is signed, but not notarized
by Apple, so macOS won't open it until the download's quarantine mark is taken off:

```sh
ditto -x -k Bada-<version>.zip ~/Library/Input\ Methods/
xattr -dr com.apple.quarantine ~/Library/Input\ Methods/Bada.app
```

Then log out and in, so that macOS finds it.

Then, in System Settings → Keyboard → Input Sources, add **bada (muhenkan)** under English and
**bada (henkan)** under Japanese, if `make install` didn't. They may not be listed until you log
out and in. For 英数 and かな to land in bada, remove the other Japanese input methods, and keep one
plain keyboard (ABC – Extended): it still types in password fields.

bada installs beside bada ime (`BadaIME.app`): no id, file or folder is shared. With both
installed, only one should have its key tap running, so allow Accessibility for one of them.

A new install has no muhenkan layout and no henkan profile. The first key that would enter henkan
(かな) opens a small window: pick the muhenkan layout and one henkan layout. That writes
`settings.json`. The input menu has "Set Up bada…" for the same, "Settings…" and "Open Settings
Folder".

**Settings…** opens the settings window: the shared [settings page](../../settings/README.md) in a
web view. Its host is the command line inside the bundle (`Contents/Helpers/bada`, started as
`bada settings --no-open --embedded` while the window is open), which serves the page on
`127.0.0.1` and reads and writes the settings folder. `build/Bada.app/Contents/MacOS/Bada
--settings` opens the window from a build, with no input method started; `BADA_HOME` names the
folder it changes.

## bada-tide

With a translator in the settings folder's `tide/` ([bada-tide](../../docs/bada-tide-v1.md) §9), the
input method starts `Bada --tide-helper`, which holds the link: it opens the keyboard's interface
(non-exclusively), keeps the lease and passes key events up a pipe; the input method tells it its
mode, profile, Caps Lock and the keys to take. A helper because in the input method's own process
(an NSApplication) macOS mostly doesn't deliver the keyboard's input reports, while sends go out. The
helper exits when the input method does, and is started again if it stops; nothing is saved.

`BADA_HOME=<folder> build/Bada.app/Contents/MacOS/Bada --tide-probe` runs the helper from a terminal
with its notes on stderr; a `{"state": {"mode": "henkan", "profile": 0, "caps": false, "take":
["Lang1"]}}` line on stdin takes keys.

## Permissions

bada asks for nothing by itself. The input menu's **Allow Language Keys Everywhere…** asks for both,
and so does starting the app with `--allow-keys`:

```sh
open -n ~/Library/Input\ Methods/Bada.app --args --allow-keys
```

| Permission | What it is for | Without it |
|---|---|---|
| **Accessibility** | The key tap: 英数 and かな in every app; bindings on other chords (`ctrl+alt+shift+meta+Space`, an F key, a modifier alone); the releases that pairs of language keys need | 英数 and かな work where macOS passes them to the input method; chords and pairs are off |
| **Input Monitoring** | Keys read at the HID level: a PC board's 無変換, 変換 and カタカナ, and Caps Lock | Those keys can't be bound |

Caps Lock needs both; without them it is macOS's own. macOS's "Use Caps Lock to switch to and from
ABC" must be off. What bada can detect now is in the state: `harness.keys.available`, `.chords`,
`.missing`.

## Files

Everything is in `~/Library/Application Support/bada` (the standard, §10.1). bada watches the
folder and reloads; a file that can't be used changes nothing, and what was wrong is in the state
(`harness.problems`) and in the input menu.

| File | What |
|---|---|
| `settings.json` | `bada-settings/2`. `engine` goes to the engine; `keys` is the key table; `harness["com.teammirae.bada.mac"]` is this harness's own |
| `layouts/*.json` | Your layouts, each fed to the engine by itself |
| `dictionary.json` | The user dictionary |
| `state.json` | Written by bada: the published state (`bada-compatible-state/2`, §6.7). Also posted as the distributed notification `com.teammirae.inputmethod.Bada.state`, with the JSON as its object |

This harness's own settings:

```json
"harness": {"com.teammirae.bada.mac": {"keyboard": "auto", "log": false}}
```

- `keyboard`: which preset a file with no `keys.bindings` gets (§9.5): `jis-mac`, `jis-pc`, `none`
  (no language keys: Caps Lock toggles), or `auto`: `jis-mac` when macOS reports a JIS keyboard,
  else `none`. macOS can't tell a PC's JIS board from Apple's, so `jis-pc` is yours to set.
- `log`: diagnostics in `~/Library/Logs/Bada.log`. Nothing typed is written.
- `apps`: per app, by bundle id. `menu: false` draws no candidate menu in that app, which shows the
  preedit itself; composition and keys are unchanged (a row can still be moved to and picked by
  key). For EVE Online, whose client draws its own composition box (issue #11; every client is
  `com.ccpgames.eveonline`):

  ```json
  "harness": {"com.teammirae.bada.mac": {"apps": {"com.ccpgames.eveonline": {"menu": false}}}}
  ```

  `bada settings set 'harness.com.teammirae.bada.mac.apps' …` can't name it, since the harness id has
  dots: set the whole part with `bada settings set harness '{"com.teammirae.bada.mac": {…}}'`, or edit
  the file.

## The sources

| | |
|---|---|
| `Sources/Core` | No AppKit, and tested: `KeyTable.swift` (the key table and pairs), `Engine.swift` (the C ABI), `Reply.swift`, `ReplyApplier.swift` (§6.3, `types`), `Settings.swift` (the folder, the onboarding's file), `PublishedState.swift`, `FolderWatcher.swift` |
| `Sources/App` | The input method: `InputController.swift` (InputMethodKit), `KeyRouter.swift` (keys from the tap, the HID reader and the input method, in order), `LanguageKeyTap.swift`, `HIDKeys.swift`, `CapsLock.swift`, `Modes.swift` (the mode, the profile, macOS's input source), `Keyboards.swift`, `SuggestionWindow.swift` (the menu), `OnboardingWindow.swift`, `StatePublisher.swift` |
| `tools` | `make-keylayout.swift` (the Keyboard Viewer's layouts, asked of the engine), `register.swift`, `login-agent.plist`, `make-icons.swift` |

The engine's data is in the app: `Contents/Resources/data` holds one file per buffer, named as the
engine names it (`kanji.bin` is the buffer `kanji`). `Bada.dataBuffers` (`Sources/App/Bada.swift`)
maps each and passes it; `make check` lists the ones that were passed.

## Not done

- The settings window has not been used from an installed copy yet: only from a build, with
  `--settings`.
- Right-to-left drawing: the harness doesn't have the `rtl` feature.
- `history/`: the onboarding window writes `settings.json` without keeping the one before. The
  settings window keeps it.
