# cli

`bada`, the command line ([the standard, §10.2](../docs/bada-compatible-v2.md#102-bada-the-command-line)):
a harness with no text field. It links the engine to check what it writes, reads and writes the
settings folder, and is the host of the [settings page](../settings/README.md) on macOS, Windows
and Linux.

```sh
cargo build --release -p bada-cli     # target/release/bada
cargo test -p bada-cli
```

It has no dependencies beyond the engine, the contract layer and `serde_json`: the HTTP server is
its own, on the standard library.

## What it does

Every command of §10.2. Every command takes `--json`: one JSON object on stdout with `ok` and
`problems` (exit code 1 when an error-level problem stopped it, 2 for a usage error). Nothing asks a
question. A change is checked as the page's host checks it and written only when it has no error;
`settings.json` keeps the file it replaced in `history/`.

| Command | Does |
|---|---|
| `bada version` | The command line's version, the engine's, the standard's, and the running input method's |
| `bada update [--check]` | Installs the newest release from GitHub, **only when run**: the download is used only if its `.sig` checks against the key in this build (below). `--check` only looks |
| `bada settings` | Serves the settings page on `127.0.0.1` and opens it (flags below) |
| `bada settings show` \| `export` | The whole file (a new install's when there is none) |
| `bada settings get <path>` \| `set <path> <json>` | One value, by path: `engine.henkan.profiles[0].layouts`. A string is JSON too: `'"en-qwerty"'` |
| `bada settings validate` \| `import <file>` \| `undo` | Check the file; replace it with another (checked); go back to the one the last write replaced (a second `undo` goes forward again) |
| `bada profile list` \| `add <id> --layouts a,b [--name N]` \| `remove <id>` \| `move <id> <n>` \| `set <id> <field> <json>` | The henkan profiles (§8). `move` counts from 0; `set` with `null` removes a field |
| `bada keys list` \| `bind <key> <action>… [--when henkan\|muhenkan]` \| `unbind <key> [--when …]` \| `available` | The key table (§9). `bind` replaces the binding on that key and `when`. `available` is what the running harness detects |
| `bada layout list` \| `show <id>` \| `add <file>` \| `set <id> <path> <json>` \| `remove <id>` \| `reset <id>` \| `copy <id> <new-id>` \| `validate <file>` \| `missing <id>` | Layouts (§7). A change to a stock layout needs `--modify-stock`, and says why without it (§7.7) |
| `bada dict show` \| `add <word> [--negative] [--language …]` \| `remove <word>` \| `import <file>` | The user dictionary (§11). A word is on one side: adding it to one takes it off the other |
| `bada type [--profile <id>] "<keys>"` | Types on the engine with no text field, with the folder's settings, layouts and dictionary, and prints the text, what is staged (`[…]`) and the menu. Without `--profile`, in muhenkan |
| `bada state` | The published state (§6.7), or `null` |
| `bada schema settings` \| `layout` \| `keys` | The JSON Schemas |

**`type`'s keys**: each character is typed as on a JIS keyboard (`a`, `A` with ⇧, `!` as ⇧1, a space
as Space, a newline as Enter); `{Code}` sends a key by its code (`{Backspace}`, `{F7}`), with
modifiers as `{shift+Code}`; `{action:menu.next}` runs an action, `{pick:2}` picks a menu row; `{{` and
`}}` type a brace. The engine's data comes from `$BADA_DATA`, a `data` folder beside the binary (the
Mac bundle's `Resources/data`, `share/bada/data`), or the repository's `engine/data`.

**`update`**: each release file has a `<file>.sig` beside it, its Ed25519 signature, made in CI by
[`tools/sign`](../tools/sign) with the secret `BADA_UPDATE_SIGNING_KEY`. The public key is
`PUBLIC_KEY` in `src/update.rs`; a build without one can check for a release but not install it.
On macOS it replaces `~/Library/Input Methods/Bada.app` and restarts the input method; on Windows it
runs the installer silently; on Linux, where the package goes under `/usr`, it checks the download and
prints the `sudo` command.

`bada settings` takes:

| Flag | |
|---|---|
| `--no-open` | Only prints the address. The first line of its output is always the address, `http://127.0.0.1:<port>/?t=<token>`: a program that starts it reads that line |
| `--embedded` | The page is in the input method's own window: `info` says `embedded: true` |
| `--port N` | The port to serve on. Without it, one of the system's choosing |
| `--page DIR` | The folder of the page's files |

## Where things are

- **The settings folder** (§10.1): `~/Library/Application Support/bada`, `%APPDATA%\bada`,
  `$XDG_CONFIG_HOME/bada` or `~/.config/bada`. `BADA_CONFIG_DIR` names another, as it does for the
  Windows and Linux input methods, and `BADA_HOME` as it does for the Mac's.
- **The page's files**: `--page`, then `$BADA_SETTINGS_PAGE`, then a folder beside the binary:
  `settings`, `../Resources/settings` (the Mac's bundle) or `../share/bada/settings` (a prefix).
  Run from the repository, it is `settings/src`. The folder is the one that has `index.html`.
- **The published state**: `state.json` in the settings folder on macOS;
  `$XDG_RUNTIME_DIR/bada/state.json` on Linux; the shared memory block on Windows, read as its
  layout says a reader does (`harness/windows/src/core/state_block.h`).

## The page's host

[settings/README.md](../settings/README.md) has the interface. What this host adds to it:

- **The token** is 128 bits, made for the run. An `/api` request without it is refused (403), and
  so is any request whose `Host` isn't `127.0.0.1:<port>` or `localhost:<port>`, or whose `Origin`
  is another site's. The page's own files need no token.
- **A check is made on a session of its own**, with the folder's layouts and no data, and thrown
  away: nothing in a check reaches a file or the running input method.
- **`keys` is checked here** (`src/keys.rs`), as each harness checks it when it reads the file:
  `unknown-field`, `bad-value` and `unknown-action` are errors; a profile, a layout or a layout's
  action that isn't there is a warning (`unknown-profile`, `unknown-layout`, `unknown-action`).
- **History names are in UTC**: `history/settings-<yyyymmdd>-<hhmmss>.json`. Two saves in one
  second take two names. Saving the text the file already has changes nothing, and keeps nothing.
- **A name that is refused** answers `ok: false` with one problem at `name`, for `write`, `remove`
  and `undo` alike.
- A layout's file must be `layouts/<id>.json` for its own `id`; another id is `bad-value` at `id`.

| File | |
|---|---|
| `src/main.rs` | The commands |
| `src/server.rs` | The server, and the host's interface |
| `src/folder.rs` | The folder: its files, a write, a removal, an undo, the history |
| `src/check.rs` | What is wrong with a settings file, a layout or a dictionary, by the engine |
| `src/keys.rs` | What is wrong with `keys` |
| `src/state.rs` | The published state, read |
| `src/page.rs` | Where the page's files are |
| `tests/host.rs` | The checks, the folder and the server, end to end |

## Not written yet

Of the standard's §10.2: `bada update`; `bada settings show`, `get`, `set`, `import`, `export` and
`undo` (the page's host can undo; the command can't yet); `bada profile`; `bada keys`;
`bada layout`; `bada dict`; `bada type`; `bada schema`. `--json` is on `version` and
`settings validate` only.

On Windows `bada.exe` is a console program: started from a shortcut it shows a console window
while it serves.
