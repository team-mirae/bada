# tools/conformance

`bada-conformance` loads any bada-compatible v2 engine and replays **transcripts** on it. It knows
nothing of the engine but the standard's eight functions.

```sh
cargo build -p bada-compat --example shout
cargo run -p bada-conformance -- target/debug/examples/libshout.dylib shout \
    tools/conformance/transcripts/any-engine.jsonl tools/conformance/transcripts/shout.jsonl
```

`--data name=path` passes a file as a named buffer to `P_data_new`, any number of times. The exit
code is 0 when every line passed, 1 when one failed, 2 when the engine couldn't be loaded.

## A transcript

JSON Lines (`bada-transcript/1`), one step a line:

| Line | Does |
|---|---|
| `{"transcript": "bada-transcript/1", "name": …}` | Names the transcript |
| `{"note": …}` | Nothing: a comment |
| `{"describe": {…}}` | Checks `P_describe`'s answer |
| `{"init": {…}}` | Starts a session with this `init_json` (the standard, §3.3), ending the one before |
| `{"new": "a"}`, `{"free": "a"}` | Makes and frees an engine, by a name of the transcript's own |
| `{"to": "a", "send": {…}, "expect": {…}}` | Sends an event to that engine. Without `to`, to the session alone |
| `{"send_raw": "{", …}` | The same with the very text, for what isn't JSON |

`expect` names only the fields that matter: an object matches when every field it has matches, an
array must match item for item, and `null` also matches a missing field. A line with no `expect`
only sends.

The same transcripts are for a harness's own tests of how it applies replies (the standard, §6.3).

## Transcripts

- `transcripts/any-engine.jsonl`: what every engine answers, whatever it types: refusals and
  their codes, `state`, `flush`, `reset`.
- `transcripts/shout.jsonl`: `shout`, the contract layer's example engine
  (`crates/compat/examples/shout.rs`).
