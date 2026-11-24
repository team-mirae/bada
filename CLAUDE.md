# bada

A new input method by Mirae Research, the next generation of bada ime: one engine (Rust), a harness
per platform (macOS, Windows, Linux, web) and a command line, in one repository, `team-mirae/bada`.
The *engine* turns keys into text; a *harness* is the platform program around it. The repository
holds the standard, its schemas, the stock layout files, the key-table cases, the contract layer
(`crates/compat`), the engine, and the four harnesses. Not here yet: the command line (`cli/`) and
the settings page (`settings/`). `docs/design.md` §13 has the order of work, and `README.md` says
what each folder holds.

## Read first

- `docs/design.md`: the design. Every decision is settled and listed in its §14.
- `docs/bada-compatible-v2.md`: the standard, written from the design. **The standard leads**:
  a change goes to `docs/` first, then to the code.
- `docs/layouts.html`: the eight typing layouts drawn on a JIS keyboard (the three `uni-*` search
  layouts have no key map of their own).
- `docs/reference/`: what bada grows from, kept as it was. bada-compatible v1, bada ime 0.3.1's spec
  (the design cites it by section) and its layout pages, and the user dictionary's contract, which
  bada keeps as v1.

## Rules

- **Nothing that already exists is touched.** The repositories under `~/Projects/` (`bada-ime`,
  `bada-compatible`, `bada-windows`, `bada-linux`, `pado-ime`, `ara-ime`, `jis-korean`,
  `jis-moonshot`, `superkeyboard`) are read-only references: read them in place, copy what is
  needed here, change nothing there. bada ime's engine and data are in `bada-ime/engine`; the
  harnesses to port from are `bada-ime/mac`, `bada-windows`, `bada-linux` and `bada-ime/web`.
- **Where the design and bada-compatible v1 disagree, the design wins.** Where the design is silent,
  v1's rule holds, and the standard says so.
- **Commit and push only when asked.** The repository is public.
- **The harness decides what language keys and Caps Lock do**; the engine only sees what its harness
  feeds it. Each harness reads the key table with its own code: no shared crate for it.
- **A layout is its JSON file, all of it.** No code for any one layout: a rule that belongs to a
  layout goes in its `bada-layout/2` file, and into the schema and the standard's §7.5 if the
  vocabulary lacks it.
- The new input method is `bada` under `com.teammirae`, publisher Mirae Research.
- Layout ids are slugs (`ko-2-set`). No integer ids anywhere.

## Branches

- `dev-latest` is the default branch. It can't be force-pushed, and pull requests into it are
  squash-merged only.
- Work happens on a `dev-*` branch and reaches `dev-latest` by pull request.

## Versions

- One version number for the whole repository, in `VERSION`: the engine, the command line and
  every harness share it. It starts at 0.1.0.
- A release carries that number on every platform it ships for. A platform released less often
  skips numbers; that is expected, not a mistake to correct.
- A release is one GitHub release, tagged `v<version>`, with every platform's build attached:
  the Windows installer (`harness/windows/tools/package.ps1`), the Mac app (`make dist` in
  `harness/mac`, or the zip CI builds) and the Linux build (`harness/linux/tools/package.sh`).
- **A published release can't be changed**: the repository's releases are immutable. So the order
  is: create the release as a draft, push the tag (CI attaches the Mac zip to the draft), attach
  the other builds, and publish last.
- The standard's version (bada-compatible 2.0) and the file formats' (`bada-settings/2`,
  `bada-layout/2`) are separate, and change only when the standard does.

## Checking

```sh
uv run tools/validate/validate.py
```

Run it after any change to `docs/schemas/`, `engine/layouts/` or `docs/cases/`. It checks shapes and
references; it runs no engine. For the code:

```sh
cargo test --workspace
cargo clippy --workspace --all-targets
```

`tools/conformance` replays transcripts on a built engine (its README has the command). Each
harness's README has its own build and test commands. When the key table's rules change (the
standard's §9), the cases in `docs/cases/key-table.json` change with them.

To redraw `docs/layouts.html`:

```sh
python3 -I tools/layouts-page/make_layouts_page.py docs/reference/layouts docs/layouts.html tools/layouts-page/layouts_template.html
```

## Writing

Plain, short sentences, in the style of `docs/design.md` and bada-compatible v1. Say what a thing
does; leave out what it used to be unless the reader needs it.
