# bada

A new input method by Mirae Research: one engine (Rust), a harness per platform (macOS, Windows,
Linux, web) and a command line, in one repository, `team-mirae/bada`. The *engine* turns keys into
text; a *harness* is the platform program around it. So far the repository holds the standard, its
schemas, the stock layout files, the key-table cases and the contract layer (`crates/compat`); the
engine and the harnesses are not written yet (`docs/design.md` §13 has the order of work).

## Read first

- `docs/design.md`: the design. Every decision is settled and listed in its §14.
- `docs/bada-compatible-v2.md`: the standard, written from the design. **The standard leads**:
  a change goes to `docs/` first, then to the code.

## Rules

- **Nothing that already exists is touched.** The repositories under `~/Projects/` (`bada-ime`,
  `bada-compatible`, `bada-windows`, `bada-linux`, `pado-ime`, `ara-ime`, `jis-korean`,
  `jis-moonshot`, `superkeyboard`) are read-only references: read them in place, copy what is
  needed here, change nothing there.
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
- Work happens on a `dev-*` branch (the first is `dev-init`) and reaches `dev-latest` by pull request.
- **`dev-init` goes to `dev-latest` only once all four harnesses work** (macOS, Windows, Linux, web):
  no pull request before that.

## Versions

- One version number for the whole repository, in `VERSION`: the engine, the command line and
  every harness share it. It starts at 0.1.0.
- A release carries that number on every platform it ships for. A platform released less often
  skips numbers; that is expected, not a mistake to correct.
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

`tools/conformance` replays transcripts on a built engine (its README has the command). When the key table's rules change (the standard's §9), the cases in
`docs/cases/key-table.json` change with them.

## Writing

Plain, short sentences, in the style of `docs/design.md` and bada-compatible v1. Say what a thing
does; leave out what it used to be unless the reader needs it.
