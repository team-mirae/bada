<p align="center"><img src="docs/logo/bada-icon.svg" width="96" alt="bada"></p>

# bada

bada (바다, バダ) is an input method by Mirae Research for Korean, Japanese and English on JIS
keyboards: one engine, a harness for each platform, and a command line. It is the next generation of
[bada ime](https://github.com/team-mirae/bada-ime), which stays as it is.

**It is being built.** Version 0.1.0 so far is the standard, the contract layer and an engine that
types every layout from its file; the harnesses are in progress, and no release exists.

## What is here

| Path | What |
|---|---|
| [docs/bada-compatible-v2.md](docs/bada-compatible-v2.md) | The standard: the interface between the engine and a harness |
| [docs/schemas/](docs/schemas/) | JSON Schemas for an event, a reply, the settings file, a layout file, `P_describe`'s answer and the key-table cases, with examples |
| [docs/cases/](docs/cases/) | Test cases for the harness's key table |
| [docs/design.md](docs/design.md) | The design the standard was written from |
| [docs/layouts.html](docs/layouts.html) | The typing layouts, drawn on a JIS keyboard |
| [docs/reference/](docs/reference/) | bada-compatible v1, bada ime's spec and layout pages, and the user dictionary's contract |
| [engine/layouts/](engine/layouts/) | The eleven stock layouts, each a `bada-layout/2` file |
| [crates/compat/](crates/compat/) | The contract layer (Rust): the standard's types, the C ABI, the session and the panic guard, with an example engine |
| [engine/](engine/) | The engine (Rust), with its data |
| `cli/`, `harness/`, `settings/` | Where the command line, the four harnesses and the settings page will live |
| [tools/](tools/) | The validator, the conformance runner, and the script that draws the layouts page |

## Checking

```sh
uv run tools/validate/validate.py
```

checks every schema, example, stock layout and case, and

```sh
cargo test --workspace
```

runs the contract layer's tests. [tools/conformance](tools/conformance/) replays transcripts on a built
engine. To redraw the layouts page:

```sh
python3 -I tools/layouts-page/make_layouts_page.py docs/reference/layouts docs/layouts.html tools/layouts-page/layouts_template.html
```

## Versions

One version number covers everything in this repository: the engine, the command line and every
harness. It is in [VERSION](VERSION), and starts at 0.1.0. A release carries that number on every
platform it ships for, so a platform that is released less often skips numbers: if macOS ships
0.1.0, 0.2.0 and 0.3.0 and Windows ships next with 0.3.0, Windows goes from 0.1.0 to 0.3.0.

The standard has its own version (bada-compatible 2.0), and so do the file formats
(`bada-settings/2`, `bada-layout/2`).

## License

MIT, copyright Mirae Research: [LICENSE](LICENSE). The files under `docs/reference/` keep the
licenses beside them.
