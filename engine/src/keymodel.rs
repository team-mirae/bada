//! The key models (the standard, §7.7; bada ime's spec §7.0): for each stock layout that can share a
//! profile, a key n-gram model of its language typed on it. The symbols are the 48 typing keys, the
//! same 48 with Shift, and the boundary before and after a word; a model says how likely each key is
//! after the three before it. The buffer is `key-models` (`engine/data/key-models.bin`).
//!
//! The ranker scores the staged keys under each enabled layout's model and turns the scores into a
//! posterior over those layouts (engine.rs, `recompute`): the layout the keys look like wins.
//!
//! File, little-endian: `BKM1`, u32 version 1, u32 order, u32 alphabet (97), u32 models; per
//! model u32 layout id and u32 length of its tables in bytes; then each model's tables: 97 costs
//! (one symbol alone), and for every longer n-gram length: u32 histories, u32 continuations, the
//! history keys (u32 each, ascending: the symbols as base-97 digits, oldest first), a backoff cost
//! per history, a continuation count per history, then per continuation its symbol (ascending
//! within a history) and its cost. Costs are −log2 of a probability in 1/16 bit, one byte.
//! Smoothing is Witten–Bell; n-grams that add little are left out and found through the backoff.

use crate::layout::{Layer, Press};

/// Symbols: 0–47 the keys, 48–95 the same with Shift, 96 the boundary.
pub const ALPHABET: usize = 97;
pub const SHIFTED: u8 = 48;
pub const BOUNDARY: u8 = 96;
/// Cost units per bit.
const STEP: f64 = 16.0;

/// The layouts the models of bada ime 0.3.1's file belong to: it names them by their old numbers.
const LEGACY: [(u32, &str); 5] =
    [(0, "en-qwerty"), (10, "ko-2-set"), (15, "ko-bada-roman"), (20, "ja-kana"), (21, "ja-romaji")];

/// N-grams of one length: histories with their continuations.
struct Level {
    /// Each history's symbols as one number, ascending.
    keys: Vec<u32>,
    backoffs: Vec<u8>,
    /// History `i`'s continuations are `symbols[first[i]..first[i + 1]]`, ascending.
    first: Vec<u32>,
    symbols: Vec<u8>,
    costs: Vec<u8>,
}

struct Model {
    layout: u32,
    unigrams: Vec<u8>,
    /// `levels[n]`: histories of n + 1 symbols.
    levels: Vec<Level>,
}

pub struct KeyModels {
    models: Vec<Model>,
}

/// A press as a symbol: its key, with or without Shift. None outside the 48 keys, and for a press
/// with ⌥, which says nothing about which layout is meant.
pub fn symbol(p: Press) -> Option<u8> {
    match p.layer {
        Layer::Base | Layer::Shift if p.key.is_typing() => {
            Some(p.key as u8 + if p.layer == Layer::Shift { SHIFTED } else { 0 })
        }
        _ => None,
    }
}

/// A history's key: its symbols as base-97 digits, oldest first.
pub fn history_key(symbols: &[u8]) -> u32 {
    symbols.iter().fold(0, |k, &s| k * ALPHABET as u32 + s as u32)
}

struct Reader<'a> {
    data: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    fn bytes(&mut self, n: usize) -> Option<&'a [u8]> {
        let out = self.data.get(self.at..self.at.checked_add(n)?)?;
        self.at += n;
        Some(out)
    }

    fn u32(&mut self) -> Option<u32> {
        Some(u32::from_le_bytes(self.bytes(4)?.try_into().ok()?))
    }
}

impl KeyModels {
    pub fn parse(data: &[u8]) -> Option<KeyModels> {
        let mut r = Reader { data, at: 0 };
        if r.bytes(4)? != b"BKM1" || r.u32()? != 1 {
            return None;
        }
        let order = r.u32()? as usize;
        if !(1..=5).contains(&order) || r.u32()? as usize != ALPHABET {
            return None;
        }
        let count = r.u32()? as usize;
        let mut headers = Vec::new();
        for _ in 0..count {
            headers.push((r.u32()?, r.u32()? as usize));
        }
        let mut models = Vec::new();
        for (layout, length) in headers {
            let end = r.at.checked_add(length)?;
            let unigrams = r.bytes(ALPHABET)?.to_vec();
            let mut levels = Vec::new();
            for _ in 1..order {
                let (histories, continuations) = (r.u32()? as usize, r.u32()? as usize);
                let keys: Vec<u32> = r
                    .bytes(histories.checked_mul(4)?)?
                    .chunks_exact(4)
                    .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                    .collect();
                // Ascending, for the binary search.
                if !keys.windows(2).all(|w| w[0] < w[1]) {
                    return None;
                }
                let backoffs = r.bytes(histories)?.to_vec();
                let mut first = vec![0u32];
                for &n in r.bytes(histories)? {
                    first.push(first.last()? + n as u32);
                }
                if *first.last()? as usize != continuations {
                    return None;
                }
                levels.push(Level {
                    keys,
                    backoffs,
                    first,
                    symbols: r.bytes(continuations)?.to_vec(),
                    costs: r.bytes(continuations)?.to_vec(),
                });
            }
            if r.at != end {
                return None;
            }
            models.push(Model { layout, unigrams, levels });
        }
        (r.at == data.len()).then_some(KeyModels { models })
    }

    /// −log2 P(`next` | `history`) in 1/16 bit: the longest history that lists `next`, with the
    /// backoff cost of each longer one that doesn't.
    fn cost(model: &Model, history: &[u8], next: u8) -> u32 {
        let mut total = 0;
        for start in 0..history.len() {
            let h = &history[start..];
            let Some(level) = model.levels.get(h.len() - 1) else { continue };
            let Ok(i) = level.keys.binary_search(&history_key(h)) else { continue };
            let range = level.first[i] as usize..level.first[i + 1] as usize;
            if let Ok(at) = level.symbols[range.clone()].binary_search(&next) {
                return total + level.costs[range.start + at] as u32;
            }
            total += level.backoffs[i] as u32;
        }
        total + model.unigrams[next as usize] as u32
    }

    /// How well `layout`'s model explains `symbols` typed from the start of a word, in nats:
    /// ln P(the keys | a word starts), and ln P(the word ends here | the keys). None without a model
    /// for the layout.
    pub fn score(&self, layout: &str, symbols: &[u8]) -> Option<(f64, f64)> {
        let id = LEGACY.iter().find(|l| l.1 == layout)?.0;
        let model = self.models.iter().find(|m| m.layout == id)?;
        let context = model.levels.len();
        let mut sequence = Vec::with_capacity(symbols.len() + 1);
        sequence.push(BOUNDARY);
        sequence.extend_from_slice(symbols);
        let at = |i: usize, next: u8| Self::cost(model, &sequence[i.saturating_sub(context)..i], next);
        let keys: u32 = (1..sequence.len()).map(|i| at(i, sequence[i])).sum();
        let end = at(sequence.len(), BOUNDARY);
        let nats = |cost: u32| -(cost as f64) / STEP * std::f64::consts::LN_2;
        Some((nats(keys), nats(end)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bada_compat::Key;

    /// A file with one model for layout 10: every symbol alone costs `unigram`; after the boundary,
    /// symbol 0 costs `listed` and the backoff costs `backoff`.
    fn file(unigram: u8, listed: u8, backoff: u8) -> Vec<u8> {
        let mut tables = vec![unigram; ALPHABET];
        for v in [1u32, 1, BOUNDARY as u32] {
            tables.extend(v.to_le_bytes());
        }
        tables.extend([backoff, 1, 0, listed]);
        let mut out = b"BKM1".to_vec();
        for v in [1u32, 2, ALPHABET as u32, 1, 10, tables.len() as u32] {
            out.extend(v.to_le_bytes());
        }
        out.extend(tables);
        out
    }

    #[test]
    fn symbols_are_the_keys_with_shift() {
        let p = |key, layer| Press { key, layer };
        assert_eq!(symbol(p(Key::Digit1, Layer::Base)), Some(0));
        assert_eq!(symbol(p(Key::IntlRo, Layer::Base)), Some(47));
        assert_eq!(symbol(p(Key::Digit1, Layer::Shift)), Some(48));
        assert_eq!(symbol(p(Key::IntlRo, Layer::Shift)), Some(95));
        assert_eq!(symbol(p(Key::Space, Layer::Base)), None);
        assert_eq!(symbol(p(Key::Digit1, Layer::AltShift)), None);
    }

    #[test]
    fn scores_with_backoff() {
        let m = KeyModels::parse(&file(32, 16, 8)).unwrap();
        let bits = |x: (f64, f64)| (x.0 / std::f64::consts::LN_2, x.1 / std::f64::consts::LN_2);
        // Symbol 0 after the boundary is listed: 1 bit; the end after it isn't: its 2 bits alone.
        assert_eq!(bits(m.score("ko-2-set", &[0]).unwrap()), (-1.0, -2.0));
        // Symbol 1 after the boundary isn't: the backoff's half bit and its 2 bits alone.
        assert_eq!(bits(m.score("ko-2-set", &[1]).unwrap()), (-2.5, -2.0));
        assert_eq!(bits(m.score("ko-2-set", &[0, 1]).unwrap()), (-3.0, -2.0));
        assert_eq!(m.score("en-qwerty", &[0]), None);
        assert_eq!(m.score("ko-my-2-set", &[0]), None, "a layout with a new id has no model");
    }

    #[test]
    fn refuses_what_isnt_a_model() {
        let good = file(32, 16, 8);
        assert!(KeyModels::parse(&good[..good.len() - 1]).is_none());
        let mut other = good.clone();
        other[..4].copy_from_slice(b"JKR4");
        assert!(KeyModels::parse(&other).is_none());
    }

    #[test]
    fn bada_imes_models_parse() {
        let bytes = std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/data/key-models.bin")).unwrap();
        let m = KeyModels::parse(&bytes).expect("engine/data/key-models.bin");
        for layout in ["en-qwerty", "ko-2-set", "ko-bada-roman", "ja-kana", "ja-romaji"] {
            assert!(m.score(layout, &[0]).is_some(), "{layout}");
        }
        assert!(m.score("en-dvorak", &[0]).is_none());
    }
}
