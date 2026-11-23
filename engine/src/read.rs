//! The general ways of reading keys (the standard, §7.5.2–§7.5.6). Each is driven entirely by the
//! layout's file: there is no code here for any one layout.

use std::collections::{BTreeMap, HashSet};

use crate::hangul::compose;
use crate::layout::{Layout, Press, Read, Role, Slot, Syllable, Table};

/// How a layout reads the staged presses.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Reading {
    /// The default reading: what the layout types for these keys.
    pub text: String,
    /// Every other reading (a `syllable` table's), for the ranking.
    pub others: Vec<String>,
    /// What kind of text it is, for `rank` (§7.5.10).
    pub shape: Shape,
    /// A text slot came before letters, in a layout whose text ends the word.
    pub after_text: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Shape {
    /// `direct`: letters only.
    Letters,
    /// `direct`: anything else. Also a search as typed.
    #[default]
    Other,
    /// Hangul in whole syllables.
    Whole,
    /// Hangul with loose jamo left over.
    Loose,
    /// Kana.
    Kana,
    /// Kana with a mark left over, or Latin letters pending.
    Broken,
}

/// The layout's slots for the presses, or None if it has none for one of them: a layout reads the
/// staged keys only if it has a slot for every one (§7.5.1).
fn slots<'a>(layout: &'a Layout, presses: &[Press]) -> Option<Vec<&'a Slot>> {
    presses.iter().map(|&p| layout.slot(p)).collect()
}

fn is_jamo(c: char) -> bool {
    ('\u{3131}'..='\u{3163}').contains(&c)
}

/// Reads the presses with a layout. None: the layout has no slot for one of them.
pub fn read(layout: &Layout, presses: &[Press], shortcuts: Option<&BTreeMap<String, Vec<String>>>) -> Option<Reading> {
    let slots = slots(layout, presses)?;
    let file = &layout.file;
    Some(match file.read {
        Read::Direct => {
            let text: String = slots.iter().map(|s| s.text.as_str()).collect();
            let letters = !slots.is_empty() && slots.iter().all(|s| s.role == Role::Letter);
            Reading { text, shape: if letters { Shape::Letters } else { Shape::Other }, ..Reading::default() }
        }
        Read::Search => {
            let text: String = slots.iter().map(|s| s.text.to_lowercase()).collect();
            Reading { text, shape: Shape::Other, ..Reading::default() }
        }
        Read::Hangul => {
            let text = hangul(file.hangul.as_ref()?, &slots);
            let shape = if text.chars().any(is_jamo) { Shape::Loose } else { Shape::Whole };
            Reading { text, shape, ..Reading::default() }
        }
        Read::Kana => {
            let marks = &file.kana.as_ref()?.marks;
            let mut text = String::new();
            for s in &slots {
                let made = (s.role == Role::Mark)
                    .then(|| text.chars().last().and_then(|last| marks.get(&s.text)?.get(&last.to_string())))
                    .flatten();
                match made {
                    Some(made) => {
                        text.pop();
                        text.push_str(made);
                    }
                    None => text.push_str(&s.text),
                }
            }
            let broken = text.chars().any(|c| marks.contains_key(&c.to_string()) || c.is_ascii_alphabetic());
            Reading { text, shape: if broken { Shape::Broken } else { Shape::Kana }, ..Reading::default() }
        }
        Read::Table => match file.table.as_ref()? {
            Table::Spellings { spellings, rules } => {
                let text = spell(spellings, rules, &slots);
                let broken = text.chars().any(|c| c.is_ascii_alphabetic());
                Reading { text, shape: if broken { Shape::Broken } else { Shape::Kana }, ..Reading::default() }
            }
            Table::Syllable { syllable } => {
                let pieces = pieces(syllable, &slots);
                let mut all = readings(syllable, &pieces, shortcuts, 4096);
                let text = if all.is_empty() { loose(syllable, &pieces) } else { all.remove(0) };
                let shape = if text.chars().any(is_jamo) { Shape::Loose } else { Shape::Whole };
                // Letters after a text slot: alone, the layout would have ended the word there.
                let after_text = file.text_ends_word
                    && pieces.iter().enumerate().any(|(i, p)| matches!(p, Piece::Text(_)) && i + 1 < pieces.len());
                Reading { text, others: all, shape, after_text }
            }
        },
    })
}

// ---------------------------------------------------------------- hangul

#[derive(Default)]
struct Syl {
    ini: Option<char>,
    vowel: Option<char>,
    /// Finals, each with whether the slot that typed it has an initial form too.
    fins: Vec<(char, bool)>,
    text: Option<String>,
}

fn tense(j: char) -> Option<char> {
    Some(match j {
        'ㄱ' => 'ㄲ',
        'ㄷ' => 'ㄸ',
        'ㅂ' => 'ㅃ',
        'ㅅ' => 'ㅆ',
        'ㅈ' => 'ㅉ',
        _ => return None,
    })
}

/// §7.5.3: jamo compose into syllables, by the layout's tables.
fn hangul(h: &crate::layout::Hangul, slots: &[&Slot]) -> String {
    let pair = |table: &BTreeMap<String, String>, a: char, b: char| {
        table.get(&format!("{a}{b}")).and_then(|s| s.chars().next())
    };
    let mut syls: Vec<Syl> = Vec::new();
    for slot in slots {
        let Some(j) = slot.text.chars().next().filter(|_| slot.text.chars().count() == 1 && slot.role != Role::Text)
        else {
            syls.push(Syl { text: Some(slot.text.clone()), ..Syl::default() });
            continue;
        };
        let (has_ini, has_fin) = match slot.role {
            Role::Consonant => (true, true),
            Role::Initial => (true, false),
            Role::Final => (false, true),
            _ => (false, false),
        };
        if slot.role == Role::Vowel {
            if let Some(cur) = syls.last_mut().filter(|c| c.text.is_none()) {
                if cur.ini.is_some() && cur.vowel.is_none() {
                    cur.vowel = Some(j);
                    continue;
                }
                if cur.fins.is_empty() {
                    if let Some(joined) = cur.vowel.and_then(|a| pair(&h.vowels, a, j)) {
                        cur.vowel = Some(joined);
                        continue;
                    }
                }
                // A vowel after a final takes it as the next syllable's initial.
                if h.final_moves_on && cur.vowel.is_some() && cur.fins.last().is_some_and(|f| f.1) {
                    let (moved, _) = cur.fins.pop().expect("a final");
                    syls.push(Syl { ini: Some(moved), vowel: Some(j), ..Syl::default() });
                    continue;
                }
            }
            syls.push(Syl { ini: h.implied_ieung.then_some('ㅇ'), vowel: Some(j), ..Syl::default() });
            continue;
        }
        if has_fin {
            if let Some(cur) = syls.last_mut().filter(|c| c.text.is_none() && c.vowel.is_some() && c.ini.is_some()) {
                if let ([(a, _)], true, false) = (cur.fins.as_slice(), h.double_tense, has_ini) {
                    if let Some(t) = tense(j).filter(|t| *a == j && matches!(t, 'ㄲ' | 'ㅆ')) {
                        cur.fins = vec![(t, false)];
                        continue;
                    }
                }
                let joins = match cur.fins.as_slice() {
                    [] => true,
                    [(a, _)] => pair(&h.finals, *a, j).is_some(),
                    _ => false,
                };
                if joins {
                    cur.fins.push((j, has_ini));
                    continue;
                }
            }
        }
        if has_ini && h.double_tense {
            if let Some(cur) = syls.last_mut().filter(|c| c.text.is_none() && c.vowel.is_none() && c.ini == Some(j)) {
                if let Some(t) = tense(j) {
                    cur.ini = Some(t);
                    continue;
                }
            }
        }
        if has_ini {
            syls.push(Syl { ini: Some(j), ..Syl::default() });
        } else {
            syls.push(Syl { text: Some(slot.text.clone()), ..Syl::default() });
        }
    }
    let mut out = String::new();
    for s in &syls {
        if let Some(text) = &s.text {
            out.push_str(text);
            continue;
        }
        let fin = match s.fins.as_slice() {
            [] => None,
            [(a, _)] => Some(*a),
            [(a, _), (b, _)] => pair(&h.finals, *a, *b),
            _ => None,
        };
        match (s.ini, s.vowel) {
            (Some(l), Some(v)) => match compose(l, v, fin) {
                Some(c) => out.push(c),
                None => {
                    out.push(l);
                    out.push(v);
                    out.extend(s.fins.iter().map(|f| f.0));
                }
            },
            (l, v) => {
                out.extend(l);
                out.extend(v);
                out.extend(s.fins.iter().map(|f| f.0));
            }
        }
    }
    out
}

// ---------------------------------------------------------------- table, spellings

/// §7.5.4, `spellings`: the pending letters are read longest first; a rule looks ahead (`before`)
/// or leaves letters pending (`keep`).
fn spell(spellings: &BTreeMap<String, String>, rules: &[crate::layout::Rule], slots: &[&Slot]) -> String {
    struct Entry<'a> {
        letters: Vec<char>,
        out: &'a str,
        before: Option<&'a str>,
        keep: usize,
    }
    let mut entries: Vec<Entry> = rules
        .iter()
        .map(|r| Entry { letters: r.letters.chars().collect(), out: &r.out, before: r.before.as_deref(), keep: r.keep })
        .collect();
    entries.extend(spellings.iter().map(|(k, v)| Entry {
        letters: k.chars().collect(),
        out: v,
        before: None,
        keep: 0,
    }));
    let waits = |p: &[char]| {
        entries
            .iter()
            .any(|e| (e.letters.len() > p.len() && e.letters.starts_with(p)) || (e.before.is_some() && e.letters == p))
    };
    let mut out = String::new();
    let mut pending: Vec<char> = Vec::new();
    let settle = |pending: &mut Vec<char>, out: &mut String, end: bool| {
        while !pending.is_empty() {
            if !end && waits(pending) {
                return;
            }
            // The longest spelling or rule that begins what is pending, a rule that looks ahead first.
            let hit = (1..=pending.len()).rev().find_map(|n| {
                let ahead = entries.iter().find(|e| {
                    e.letters == pending[..n]
                        && e.before.is_some_and(|b| pending.get(n).is_some_and(|next| b.contains(*next)))
                });
                ahead
                    .or_else(|| entries.iter().find(|e| e.letters == pending[..n] && e.before.is_none()))
                    .map(|e| (n, e))
            });
            match hit {
                Some((n, e)) => {
                    out.push_str(e.out);
                    pending.drain(..n - e.keep.min(n - 1));
                }
                None => out.push(pending.remove(0)),
            }
        }
    };
    for slot in slots {
        if slot.role == Role::Letter {
            pending.extend(slot.text.chars().flat_map(char::to_lowercase));
            settle(&mut pending, &mut out, false);
        } else {
            settle(&mut pending, &mut out, true);
            out.push_str(&slot.text);
        }
    }
    settle(&mut pending, &mut out, true);
    out
}

// ---------------------------------------------------------------- table, syllable

#[derive(Clone, Debug, PartialEq, Eq)]
enum Piece {
    Run(Vec<char>),
    Break,
    Text(String),
}

/// Whether a slot is text to this table, after the slots staged before it: not a letter, not a
/// break, and not a text slot that `letter_after` turns into a letter.
pub fn is_text(syllable: &Syllable, staged: &[&Slot], slot: &Slot) -> bool {
    if slot.role != Role::Text {
        return false;
    }
    let before = pieces(syllable, staged);
    let last = match before.last() {
        Some(Piece::Run(run)) => run.last().copied(),
        _ => None,
    };
    !joins(syllable, last, slot)
}

fn joins(syllable: &Syllable, last: Option<char>, slot: &Slot) -> bool {
    syllable
        .letter_after
        .get(&slot.text)
        .is_some_and(|after| last.is_some_and(|l| after.iter().any(|a| a.chars().eq([l]))))
}

fn pieces(syllable: &Syllable, slots: &[&Slot]) -> Vec<Piece> {
    let mut out: Vec<Piece> = Vec::new();
    for slot in slots {
        let last = match out.last() {
            Some(Piece::Run(run)) => run.last().copied(),
            _ => None,
        };
        let letter = slot.role == Role::Letter || (slot.role == Role::Text && joins(syllable, last, slot));
        if letter {
            let letters = slot.text.chars().flat_map(char::to_lowercase);
            match out.last_mut() {
                Some(Piece::Run(run)) => run.extend(letters),
                _ => out.push(Piece::Run(letters.collect())),
            }
        } else if slot.role == Role::Break {
            out.push(Piece::Break);
        } else {
            out.push(Piece::Text(slot.text.clone()));
        }
    }
    out
}

/// The spellings of a table that begin `s`, the longest first.
fn matches<'a>(table: &'a BTreeMap<String, Vec<String>>, s: &[char]) -> Vec<(usize, &'a [String])> {
    let mut found: Vec<(usize, &[String])> = table
        .iter()
        .filter(|(k, _)| k.chars().count() <= s.len() && k.chars().zip(s).all(|(a, b)| a == *b))
        .map(|(k, v)| (k.chars().count(), v.as_slice()))
        .collect();
    found.sort_by_key(|f| std::cmp::Reverse(f.0));
    found
}

fn one(jamo: &str) -> Option<char> {
    jamo.chars().next()
}

/// Every way a syllable can start at `i`: its readings, the default first, and where it ends.
fn syllables_at(
    t: &Syllable,
    run: &[char],
    i: usize,
    shortcuts: Option<&BTreeMap<String, Vec<String>>>,
) -> Vec<(Vec<char>, usize)> {
    let vowel_letter = |c: char| t.vowels.keys().any(|k| k.contains(c));
    let mut out = Vec::new();
    let mut onsets = matches(&t.onsets, &run[i..]);
    if !matches(&t.vowels, &run[i..]).is_empty() {
        onsets.push((0, t.empty_onset.as_slice()));
    }
    for (on, onset) in onsets {
        let j = i + on;
        for (vn, vowel) in matches(&t.vowels, &run[j..]) {
            let k = j + vn;
            let end = (k..run.len()).find(|&x| vowel_letter(run[x])).unwrap_or(run.len());
            let cons = &run[k..end];
            let spelling: String = run[j..k].iter().collect();
            let mut codas: Vec<(usize, &[String])> = matches(&t.codas, cons);
            for c in &t.codas_before {
                let both: Vec<char> = c.coda.chars().chain(c.next.chars()).collect();
                if cons.starts_with(&both) {
                    codas.push((c.coda.chars().count(), c.jamo.as_slice()));
                }
            }
            codas.push((0, &[]));
            let last = end == run.len();
            // A coda is preferred when it leaves nothing, or leaves exactly one onset spelling.
            let preferred = |n: usize| {
                let rest: String = cons[n..].iter().collect();
                rest.is_empty() || (!last && t.onsets.contains_key(&rest))
            };
            codas.sort_by_key(|&(n, _)| if preferred(n) { (0, n as isize) } else { (1, -(n as isize)) });
            for (cn, coda) in codas {
                let vowel =
                    if cn > 0 { t.vowels_before_coda.get(&spelling).map_or(vowel, Vec::as_slice) } else { vowel };
                let finals: Vec<Option<char>> =
                    if coda.is_empty() { vec![None] } else { coda.iter().map(|c| one(c)).collect() };
                let mut syls = Vec::new();
                for l in onset.iter().filter_map(|l| one(l)) {
                    for v in vowel.iter().filter_map(|v| one(v)) {
                        for f in &finals {
                            syls.extend(compose(l, v, *f));
                        }
                    }
                }
                if !syls.is_empty() {
                    out.push((syls, k + cn));
                }
            }
        }
    }
    for (romaji, syls) in shortcuts.into_iter().flatten() {
        if run[i..].iter().copied().take(romaji.chars().count()).eq(romaji.chars()) {
            out.push((syls.iter().filter_map(|s| one(s)).collect(), i + romaji.chars().count()));
        }
    }
    out
}

/// Readings of the pieces, the default first, at most `limit`.
fn readings(
    t: &Syllable,
    pieces: &[Piece],
    shortcuts: Option<&BTreeMap<String, Vec<String>>>,
    limit: usize,
) -> Vec<String> {
    struct Walk<'a> {
        t: &'a Syllable,
        pieces: &'a [Piece],
        shortcuts: Option<&'a BTreeMap<String, Vec<String>>>,
        out: Vec<String>,
        seen: HashSet<String>,
        limit: usize,
        budget: usize,
    }
    impl Walk<'_> {
        fn go(&mut self, pi: usize, i: usize, built: &mut String) {
            if self.out.len() >= self.limit || self.budget == 0 {
                return;
            }
            self.budget -= 1;
            let Some(piece) = self.pieces.get(pi) else {
                if self.seen.insert(built.clone()) {
                    self.out.push(built.clone());
                }
                return;
            };
            match piece {
                Piece::Break => self.go(pi + 1, 0, built),
                Piece::Text(text) => {
                    built.push_str(text);
                    self.go(pi + 1, 0, built);
                    built.truncate(built.len() - text.len());
                }
                Piece::Run(run) if i == run.len() => self.go(pi + 1, 0, built),
                Piece::Run(run) => {
                    for (syls, end) in syllables_at(self.t, run, i, self.shortcuts) {
                        for s in syls {
                            built.push(s);
                            self.go(pi, end, built);
                            built.pop();
                        }
                    }
                }
            }
        }
    }
    let mut walk = Walk { t, pieces, shortcuts, out: Vec::new(), seen: HashSet::new(), limit, budget: 200_000 };
    walk.go(0, 0, &mut String::new());
    walk.out
}

/// When the letters make no reading: each syllable they do make, and the rest as the first jamo of
/// their onset, or as themselves.
fn loose(t: &Syllable, pieces: &[Piece]) -> String {
    let mut s = String::new();
    for piece in pieces {
        match piece {
            Piece::Break => {}
            Piece::Text(text) => s.push_str(text),
            Piece::Run(run) => {
                let mut i = 0;
                while i < run.len() {
                    if let Some((syls, end)) = syllables_at(t, run, i, None).into_iter().next() {
                        s.push(syls[0]);
                        i = end;
                    } else {
                        let onset = matches(&t.onsets, &run[i..i + 1]);
                        s.push(onset.first().and_then(|o| one(&o.1[0])).unwrap_or(run[i]));
                        i += 1;
                    }
                }
            }
        }
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::{Layer, jis, stock};
    use bada_compat::Key;

    fn layout(id: &str) -> std::sync::Arc<Layout> {
        stock().iter().find(|l| l.id() == id).unwrap().clone()
    }

    /// The presses that type `text` on a JIS keyboard as QWERTY has it.
    fn keys(text: &str) -> Vec<Press> {
        text.chars()
            .map(|c| {
                Key::ALL[..48]
                    .iter()
                    .find_map(|&key| {
                        [false, true]
                            .into_iter()
                            .find(|&s| jis(key, s) == Some(c))
                            .map(|s| Press { key, layer: Layer::of(s, false) })
                    })
                    .unwrap_or_else(|| panic!("no key types {c:?}"))
            })
            .collect()
    }

    fn typed(id: &str, text: &str) -> String {
        read(&layout(id), &keys(text), None).unwrap().text
    }

    // bada ime 0.3.1's own vectors (engine/src/japanese.rs, bada_roman.rs, layout.rs), typed on the
    // layout files.

    #[test]
    fn romaji() {
        for (keys, want) in [
            ("konnnichiha", "こんにちは"),
            ("konnichiha", "こんにちは"),
            ("gakkou", "がっこう"),
            ("shinbun", "しんぶん"),
            ("kyouto", "きょうと"),
            ("ra-men", "らーめん"),
            ("kan", "かん"),
            ("ky", "ky"),
            ("matcha", "まっちゃ"),
            ("arigatou.", "ありがとう。"),
            ("fairu", "ふぁいる"),
            ("konn", "こん"),
            ("annna", "あんな"),
            ("kinnyoubi", "きんようび"),
            ("tan'i", "たんい"),
            ("nihon1", "にほん１"),
            ("Kyoto", "Ｋよと"),
        ] {
            assert_eq!(typed("ja-romaji", keys), want, "{keys}");
        }
        assert_eq!(read(&layout("ja-romaji"), &keys("ky"), None).unwrap().shape, Shape::Broken);
        assert_eq!(read(&layout("ja-romaji"), &keys("ka"), None).unwrap().shape, Shape::Kana);
    }

    #[test]
    fn kana() {
        let p = |key, shift| Press { key, layer: Layer::of(shift, false) };
        let kana = layout("ja-kana");
        let read = |presses: &[Press]| read(&kana, presses, None).unwrap();
        assert_eq!(
            read(&[
                p(Key::KeyT, false),
                p(Key::BracketLeft, false),
                p(Key::KeyZ, true),
                p(Key::KeyB, false),
                p(Key::Digit4, false)
            ])
            .text,
            "がっこう"
        );
        assert_eq!(read(&[p(Key::KeyF, false), p(Key::BracketRight, false)]).text, "ぱ");
        let stray = read(&[p(Key::Digit3, false), p(Key::BracketLeft, false)]);
        assert_eq!((stray.text.as_str(), stray.shape), ("あ゛", Shape::Broken));
        let alt = |key| Press { key, layer: Layer::AltShift };
        assert_eq!(
            read(&[p(Key::KeyT, false), alt(Key::Slash), alt(Key::Digit1), alt(Key::Digit8), alt(Key::IntlRo)]).text,
            "か？！（＿"
        );
        assert!(
            super::read(&kana, &[Press { key: Key::Slash, layer: Layer::Alt }], None).is_none(),
            "⌥ alone isn't the layout's"
        );
    }

    #[test]
    fn two_set() {
        for (keys, want) in [
            ("gks", "한"),
            ("gksrnrdj", "한국어"),
            ("rhk", "과"),
            ("rkqt", "값"),
            ("rkqtk", "갑사"),
            ("Rk", "까"),
            ("gk!", "하!"),
            ("ks", "ㅏㄴ"),
            ("rt", "ㄱㅅ"),
            ("ekfr", "닭"),
            ("rkQ", "가ㅃ"),
            ("rkQk", "가빠"),
            ("¥", "₩"),
            ("|", "|"),
        ] {
            assert_eq!(typed("ko-2-set", keys), want, "{keys}");
        }
        assert_eq!(read(&layout("ko-2-set"), &keys("gks"), None).unwrap().shape, Shape::Whole);
        assert_eq!(read(&layout("ko-2-set"), &keys("rt"), None).unwrap().shape, Shape::Loose);
    }

    #[test]
    fn bada_roman() {
        for (keys, want) in [
            ("hanguk", "한국"),
            ("han;guk", "한국"),
            ("hang;uk", "항욱"),
            ("anxa", "안아"),
            ("xx", "ㅇㅇ"),
            ("e-", "애"),
            ("pe-", "패"),
            ("ye-", "얘"),
            ("we-", "왜"),
            ("dwe-", "돼"),
            ("gwe-n", "괜"),
            ("a-", "아-"),
            ("han-guk", "한-국"),
            ("Hanguk", "한국"),
            ("qa", "차"),
            ("qeong", "청"),
            ("Qa", "차"),
            ("woori", "우리"),
            ("young", "영"),
            ("eui", "의"),
            ("gaeui", "개의"),
            ("you", "유"),
            ("youngho", "영호"),
            ("joh", "좋"),
            ("its", "있"),
            ("hats", "핬"),
            ("zoh", "좋"),
            ("zza", "짜"),
            ("jja", "짜"),
            ("zzigae", "찌개"),
            ("azza", "아짜"),
            ("daltorok", "달토록"),
            ("naj", "낮"),
            ("naz", "낮"),
            ("nat", "낫"),
            ("jaj", "잦"),
            ("anj", "앉"),
            ("naja", "나자"),
            ("joha", "조하"),
            ("nyun", "년"),
            ("gyung", "경"),
            ("nyu", "뉴"),
            ("sung", "숭"),
            ("seigong", "세이공"),
            ("annyeonghaseyo", "안녕하세요"),
        ] {
            assert_eq!(typed("ko-bada-roman", keys), want, "{keys}");
        }
        let all = |text: &str| read(&layout("ko-bada-roman"), &keys(text), None).unwrap().others;
        for (keys, want) in [
            ("itseo", "있어"),
            ("daltorok", "닳도록"),
            ("silta", "싫다"),
            ("nat", "낮"),
            ("naja", "낮아"),
            ("naza", "낮아"),
            ("joha", "좋아"),
            ("kyung", "경"),
            ("nyun", "뉸"),
            ("sung", "성"),
        ] {
            assert!(all(keys).iter().any(|r| r == want), "{keys} offers {want}");
        }
        assert!(!all("salda").iter().any(|r| r == "삻다"));
        let short =
            BTreeMap::from([("mei".to_string(), vec!["명".to_string()]), ("sei".to_string(), vec!["성".to_string()])]);
        let with = |text: &str| read(&layout("ko-bada-roman"), &keys(text), Some(&short)).unwrap();
        assert!(with("mei").others.iter().any(|r| r == "명"));
        assert!(with("seigong").others.iter().any(|r| r == "성공"));
        assert_eq!(with("seigong").text, "세이공", "the default reading stays the letters'");
        assert!(!all("mei").iter().any(|r| r == "명"), "off: not offered");
        assert!(read(&layout("ko-bada-roman"), &keys("3a"), None).unwrap().after_text);
    }

    #[test]
    fn direct_and_search() {
        assert_eq!(typed("en-qwerty", "It's"), "It's");
        assert_eq!(read(&layout("en-qwerty"), &keys("the"), None).unwrap().shape, Shape::Letters);
        assert_eq!(read(&layout("en-qwerty"), &keys("U.S."), None).unwrap().shape, Shape::Other);
        // Dvorak and Colemak by position: the keys QWERTY prints `jdpr` and `hkjp;` on.
        assert_eq!(typed("en-dvorak", "jdpr"), "help");
        assert_eq!(typed("en-colemak", "hkjp;"), "hen;o");
        assert_eq!(typed("en-qwerty-us", "\"^"), "@=");
        assert_eq!(typed("uni-unicode", "U+1F600"), "u+1f600");
        assert!(
            read(&layout("en-qwerty"), &[Press { key: Key::Digit0, layer: Layer::Shift }], None).is_none(),
            "⇧0 is empty"
        );
    }
}
