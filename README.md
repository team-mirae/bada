<p align="center"><img src="docs/logo/bada-icon.svg" width="96" alt="bada"></p>

# bada

bada (바다, バダ) is an input method by Mirae Research for Korean, Japanese and English on JIS
keyboards: one engine, a harness for each platform, and a command line. It is the next generation of
[bada ime](https://github.com/team-mirae/bada-ime), which stays as it is.

**It is early.** There is the standard, the contract layer, an engine that types every layout from
its file, the four harnesses, and a settings page for all of them. The command line opens that page
(`bada settings`) and checks a settings file; its other commands are not written yet.

## Install

A [release](https://github.com/team-mirae/bada/releases) carries a build for each platform:

| Platform | File | How |
|---|---|---|
| Windows 10 and 11 (x64) | `bada-<version>-setup.exe` | Run it. [harness/windows](harness/windows/README.md) |
| macOS | `Bada-<version>.zip` | [harness/mac](harness/mac/README.md#install) |
| Linux, Fcitx5 (x86_64) | `bada-<version>-linux-x86_64.tar.gz` | `INSTALL.txt` inside. [harness/linux](harness/linux/README.md) |

The web harness is a page: `node harness/web/build.mjs` builds it ([harness/web](harness/web/README.md)).

## What is here

| Path | What |
|---|---|
| [docs/bada-compatible-v2.md](docs/bada-compatible-v2.md) | The standard: the interface between the engine and a harness |
| [docs/schemas/](docs/schemas/) | JSON Schemas for an event, a reply, the settings file, a layout file, `P_describe`'s answer and the key-table cases, with examples |
| [docs/cases/](docs/cases/) | Test cases for the harness's key table |
| [docs/design.md](docs/design.md) | The design the standard was written from |
| [docs/layouts.html](docs/layouts.html) | The typing layouts, drawn on a JIS keyboard |
| [docs/reference/](docs/reference/) | bada-compatible v1, bada ime's spec and layout pages, and the user dictionary's contract |
| [engine/layouts/](engine/layouts/) | The eleven stock layouts, each a `bada-layout/3` file |
| [crates/compat/](crates/compat/) | The contract layer (Rust): the standard's types, the C ABI, the session and the panic guard, with an example engine |
| [engine/](engine/) | The engine (Rust), with its data |
| [harness/](harness/) | The four harnesses: [mac](harness/mac/), [windows](harness/windows/), [linux](harness/linux/), [web](harness/web/) |
| [settings/](settings/) | The settings page: one page for every platform, with the interface a host gives it |
| [cli/](cli/) | `bada`, the command line: the settings page's host on macOS, Windows and Linux |
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
(`bada-settings/2`, `bada-layout/3`).

## License

MIT, copyright Mirae Research: [LICENSE](LICENSE). The files under `docs/reference/` keep the
licenses beside them.
