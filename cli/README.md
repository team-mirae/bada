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

## What it does now

| Command | Does |
|---|---|
| `bada version [--json]` | The command line's version, the engine's, the standard's, and the running input method's (from the state it publishes) |
| `bada settings` | Serves the settings page on `127.0.0.1` and opens it in the default browser. It stops when the page is closed, or hasn't been heard from for a minute |
| `bada settings validate [--json]` | Checks the folder's `settings.json` as the page's host does, and prints what is wrong. No file is a new install: nothing is wrong. Exit code 1 with an error-level problem |
| `bada state` | The published state (§6.7), or `null` when nothing is published |

Anything else prints the usage, with exit code 2.

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
