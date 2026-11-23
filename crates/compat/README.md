# crates/compat

`bada-compat`: the contract layer of [bada-compatible v2](../../docs/bada-compatible-v2.md). An
engine implements its `Engine` trait and exports it with `export!`; the layer does what the standard
asks of every engine, so no engine writes it twice.

| | |
|---|---|
| `src/key.rs` | Key codes and modifiers (the standard, §4) |
| `src/event.rs` | The eleven event types, read from JSON with their refusals (§5, §6.2); JSON merge patch |
| `src/reply.rs` | Replies, problems, and what `P_describe` answers (§6, §3.2) |
| `src/ffi.rs` | The eight C functions, the handles, the panic guard, and `export!` (§3) |
| `src/lib.rs` | The `Engine` trait, and `header()`, which writes the C header for a prefix |
| `examples/shout.rs` | `shout`, a small engine on this layer: the tests' engine, and an example to start from |
| `tests/conformance.rs` | The engine's part of the standard's checklist (§13), through the C ABI |

The layer itself answers `bad-json`, `bad-event`, `unknown-event` and `needs-engine`; keeps each
reply alive until the next `P_send`; echoes `mode` and `profile`; answers `state` from the last
reply; and catches panics, after which the engine is made anew with its last `select`.

```sh
cargo test -p bada-compat
```

A change here is a change to the standard: it goes to `docs/` first.
