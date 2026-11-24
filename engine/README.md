# engine

bada's engine (Rust): keys in, text out, through the C ABI of
[bada-compatible v2](../docs/bada-compatible-v2.md). The library is `bada` (`libbada.a`, `libbada.dylib`
or `.so`), its functions `bada_*`, its header [include/bada.h](include/bada.h).

**No layout has code of its own.** Each of the eleven stock layouts is a `bada-layout/2` file in
[layouts/](layouts/), read by a few general ways of reading keys
([the standard, §7.5](../docs/bada-compatible-v2.md#75-a-layout-file-bada-layout2)).

| | |
|---|---|
| `src/layout.rs` | A layout file: as written, checked, and ready to read keys with; the JIS table |
| `src/read.rs` | The ways of reading: `direct`, `hangul`, `table` (spellings, syllables), `kana`, `search` |
| `src/session.rs` | The session: settings, custom layouts, the user dictionary, and what was wrong with each |
| `src/field.rs` | One text field: staging, the menu, fix, actions, ranking, replies |
| `src/data.rs` | Named buffers: word lists, kanji, key models; the user dictionary |
| `src/kanji.rs`, `src/romaji.rs`, `src/keymodel.rs`, `src/width.rs`, `src/hangul.rs`, `src/script.rs` | The general machinery, from bada ime 0.3.1 |
| `data/` | bada ime 0.3.1's data, with its licenses: passed to the engine as named buffers |
| `tests/typing.rs` | Typing through the C ABI, with bada ime's own test words |

```sh
cargo test -p bada-engine
cargo run -q -p bada-engine --example header > engine/include/bada.h    # after a change to the ABI
```

## Not done yet

- **Search** (`uni-unicode`, `uni-symbols`, `uni-emojis`): the search as typed shows, but nothing is found.
- **bada roman's ranking**: its other readings, the jamo model, the romaji shortcuts and the English
  dictionary. The spelling itself types.
- **Lookups** (`ko-2-set.hanja`), **a word and its particle**, and key models for `en-qwerty-us`,
  `en-dvorak` and `en-colemak`.
- **The pack**: the data is still separate files.
