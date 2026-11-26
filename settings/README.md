# settings

The settings page: one page for every platform, drawn from the settings folder's files
([the standard, §10](../docs/bada-compatible-v2.md#10-settings-files-and-the-command-line)). It is
plain modules with no framework and no build step: `src/` is what is served.

| File | |
|---|---|
| `src/index.html`, `src/style.css` | The page as a host serves it |
| `src/page.js` | The page: `mount(root, host)`. Profiles, Keys, Layouts, Dictionary, Files, the platform's own tab, About, and the first run |
| `src/model.js` | The settings file as the page changes it, with no DOM: what a binding says, what is wrong with `keys` |
| `src/host-http.js` | The host below, over HTTP: what `bada settings` serves |
| `test/` | `node settings/test/all.mjs` |

## Hosts

The page reads and writes nothing itself. A **host** does, and every host gives the page the same
object:

| Platform | Host | Where the page shows |
|---|---|---|
| macOS | `bada settings`, started by Bada.app | A window of Bada.app (WKWebView) |
| Windows, Linux | `bada settings` | The default browser |
| Web | The web harness | Its Settings dialog. The folder is the browser's local storage |

`bada settings` serves the page on `127.0.0.1`, on a port of its own, and prints the address. The
address carries a token made for that run, and every request below must carry it.

## The host's interface

What `host-http.js` calls, and what the web harness answers itself. Every `/api` request has the
header `X-Bada-Token`; a request without the run's token, or with a `Host` or `Origin` that isn't
the page's own, is refused (403). Bodies and answers are JSON.

| Call | HTTP | Answers |
|---|---|---|
| `info()` | `GET /api/info` | `{platform, harness, version, folder, embedded}`: `mac`, `windows`, `linux` or `web`; the harness's id; bada's version; the folder as the user would write it; whether the page is in the input method's own window |
| `describe()` | `GET /api/describe` | The engine's `P_describe` answer |
| `stock()` | `GET /api/stock` | `[{id, text}]`: the stock layouts' files |
| `files()` | `GET /api/files` | `{settings, dictionary, layouts, history}`: `settings` and `dictionary` are `{text, modified}` or null when there is no file; `layouts` is `[{id, text, modified}]`, the user's own; `history` is `[{name, modified}]`, newest first. `modified` is seconds since 1970 |
| `check(what)` | `POST /api/check` | `what` is `{settings: text}`, `{layout: text}` or `{dictionary: text}`. `{ok, problems, layouts}`: `problems` are `[{path, code, message, level}]` with paths into the file; `layouts` is the session's layouts as `P_describe.layouts` has them, the user's own included. Nothing is written |
| `write(name, text)` | `POST /api/write` | `name` is `settings.json`, `dictionary.json` or `layouts/<id>.json`. Checked as `check` does, and refused (`ok: false`) with an error-level problem. Written to a temporary file and renamed. `{ok, problems, files}` |
| `remove(name)` | `POST /api/remove` | `dictionary.json` or `layouts/<id>.json`. `{ok, files}` |
| `undo(name)` | `POST /api/undo` | Puts a file of `history/` back as `settings.json`. `{ok, problems, files}` |
| `state()` | `GET /api/state` | The published state (§6.7), or null when the input method isn't running |
| `reveal()` | `POST /api/reveal` | Shows the folder in the file manager. `{ok}` |
| | `POST /api/alive` | The page is still open. The host stops when it hasn't heard this for a minute |
| | `POST /api/bye` | The page was closed: the host stops |

- **`settings.json` is checked as one file**: its `engine` part by the engine (a dry `settings`
  event, on a session that has the folder's layouts), its `keys` by the host (§9), and a part the
  format doesn't have is `unknown-field`.
- **Every save of `settings.json` keeps the file it replaced** in `history/`, as
  `settings-<yyyymmdd>-<hhmmss>.json` (the time in UTC), and the last 20 are kept. Writing the text
  the file already has writes nothing.
- **Names are checked, not trusted**: a layout's file is `layouts/<its id>.json`, an id is a slug
  (§7.1), and nothing outside the folder is read or written.
