//! One text field (the standard, §3.1): its mode, its profile and its staged keys. The key handling
//! is bada ime 0.3.1's (`docs/reference/bada-ime-spec.md` §6), with the language keys gone: what
//! they did arrives as `select` and `action` events (§5).

use std::sync::Arc;

use bada_compat::{
    Ask, Cause, Error, Key, KeyEvent, Menu, Mode, Preedit, Replace, Reply, Row, Segment, SegmentKind, Text, code,
};

use crate::data::Strength;
use crate::hangul::decompose;
use crate::keymodel::symbol;
use crate::layout::{Convert, Do, Keep, Layer, Layout, Press, Read, Role, Script, Table, WidthName, jis, key_index};
use crate::read::{Reading, Shape, is_text, read};
use crate::script::katakana;
use crate::session::{Profile, Session, Snapshot};
use crate::width::{Width, convert, full_width, half_width};

/// A layout's kanji conversion, as its file has it (§7.5.7).
struct KanjiConvert {
    data: String,
    rows: usize,
    user_rows: usize,
    /// A list of common words by reading, and how many rows it gets.
    common: Option<(String, usize)>,
    native: Option<String>,
    /// Kana rows carry their romaji; converted rows their reading.
    kana_note: bool,
    converted_note: bool,
}

impl KanjiConvert {
    fn of(convert: &Convert) -> Option<KanjiConvert> {
        let Convert::Kanji { data, rows, user_rows, common, native, notes } = convert else { return None };
        let rows = rows.unwrap_or(6);
        let common = common
            .as_ref()
            .and_then(|c| Some((c.get("data")?.as_str()?.to_string(), c.get("rows")?.as_u64()? as usize)));
        let note = |name: &str, value: &str| notes.as_ref().and_then(|n| n.get(name)?.as_str()) == Some(value);
        Some(KanjiConvert {
            data: data.clone(),
            rows,
            user_rows: user_rows.unwrap_or(rows),
            common,
            native: native.clone(),
            kana_note: note("kana", "romaji"),
            converted_note: note("converted", "reading"),
        })
    }
}

/// The key models: a layout's evidence is KEY_POINTS for every nat of its share among the layouts
/// that read the keys; a word that goes on after the last staged key counts KEY_GOES_ON beside one
/// that ends there; and no layout loses more than KEY_FLOOR.
const KEY_POINTS: f64 = 5.0;
const KEY_GOES_ON: f64 = 0.02;
const KEY_FLOOR: f64 = 60.0;
/// Where every layout's first reading starts before its evidence; more when it is a known word,
/// more again when the word is the user's own.
const SET_SCORE: f64 = 80.0;
const SET_KNOWN: f64 = 5.0;
const SET_OWN: f64 = 40.0;

/// One row of the menu, before it is written.
#[derive(Clone, Debug, Default)]
struct Entry {
    text: String,
    score: f64,
    note: Option<String>,
    lang: Option<String>,
    /// The layout that raised it.
    layout: Option<String>,
    /// It is a `direct` layout's reading: the keys as typed.
    direct: bool,
}

/// What shows in place of the ranked readings, until the commit (a layout's `keys` or `script` action).
#[derive(Clone, Debug, PartialEq, Eq)]
enum Override {
    Keys(WidthName),
    Script { layout: String, to: Script },
}

/// What the engine asked the harness's selection for.
#[derive(Clone, Debug, PartialEq, Eq)]
enum Waiting {
    Width(Width),
    Jamo(Keep),
}

/// Muhenkan's open strip of variants (§7.5.2).
#[derive(Clone, Debug)]
struct Held {
    key: Key,
    letter: String,
    variants: Vec<String>,
    selected: Option<usize>,
}

impl Held {
    fn shown(&self) -> &str {
        self.selected.map_or(self.letter.as_str(), |i| self.variants[i].as_str())
    }
}

pub struct Field {
    session: Arc<Session>,
    view: Arc<Snapshot>,
    mode: Mode,
    profile: Option<String>,
    /// The profile was removed from the settings: `no-profile` until the next `select`.
    orphan: bool,
    /// Text that no longer changes: fixed, or made by an action on what showed.
    prefix: String,
    presses: Vec<Press>,
    over: Option<Override>,
    entries: Vec<Entry>,
    /// Script forms: entry −1, −2.
    negatives: Vec<Entry>,
    /// The signed index of the entry that shows.
    index: i32,
    /// A row picked below the top, and its layout: typing on fixes it.
    chosen: Option<(String, Option<String>)>,
    /// The layout whose reading was fixed last: its reading of the keys typed after shows.
    stuck: Option<String>,
    waiting: Option<Waiting>,
    held: Option<Held>,
}

fn in_script(hiragana: &str, to: Script) -> String {
    match to {
        Script::Hiragana => hiragana.to_string(),
        Script::Katakana => katakana(hiragana),
        Script::HalfwidthKatakana => convert(&katakana(hiragana), Width::Half),
    }
}

fn width(name: WidthName) -> Width {
    match name {
        WidthName::Full => Width::Full,
        WidthName::Half => Width::Half,
    }
}

/// One jamo of each Hangul syllable; a syllable with no final is dropped for `Finals`. Everything
/// else stays as it is.
fn keep_jamo(text: &str, keep: Keep) -> String {
    text.chars()
        .filter_map(|c| match decompose(c) {
            None => Some(c),
            Some((l, v, f)) => match keep {
                Keep::Initials => Some(l),
                Keep::Vowels => Some(v),
                Keep::Finals => f,
            },
        })
        .collect()
}

impl Field {
    pub fn new(session: Arc<Session>) -> Field {
        let view = session.view();
        Field {
            session,
            view,
            mode: Mode::Muhenkan,
            profile: None,
            orphan: false,
            prefix: String::new(),
            presses: Vec::new(),
            over: None,
            entries: Vec::new(),
            negatives: Vec::new(),
            index: 0,
            chosen: None,
            stuck: None,
            waiting: None,
            held: None,
        }
    }

    // ---------- what the field reads ----------

    fn current(&self) -> Option<&Profile> {
        self.view.settings.profile(self.profile.as_deref()?)
    }

    fn candidates(&self) -> Vec<Arc<Layout>> {
        if self.mode != Mode::Henkan {
            return Vec::new();
        }
        self.profile.as_deref().map(|p| self.view.candidates(p)).unwrap_or_default()
    }

    fn muhenkan_layout(&self) -> Option<Arc<Layout>> {
        self.view.layouts.get(self.view.settings.muhenkan.layout.as_deref()?).cloned()
    }

    /// Takes the session's settings and layouts as they are now. If this field's profile was
    /// changed or removed, what it has staged is committed: the text comes back, for this event's
    /// reply (§8.2).
    fn sync(&mut self) -> String {
        let now = self.session.view();
        if now.generation == self.view.generation {
            return String::new();
        }
        let before = self.current().cloned();
        let same_layouts = |a: &Snapshot, b: &Snapshot, p: &Profile| {
            p.layouts.iter().all(|id| match (a.layouts.get(id), b.layouts.get(id)) {
                (Some(x), Some(y)) => Arc::ptr_eq(x, y),
                (None, None) => true,
                _ => false,
            })
        };
        let changed = match (&before, self.profile.as_deref().and_then(|p| now.settings.profile(p))) {
            (Some(old), Some(new)) => {
                old != new
                    || now.settings.henkan.defaults != self.view.settings.henkan.defaults
                    || !same_layouts(&self.view, &now, old)
            }
            (Some(_), None) => true,
            (None, _) => false,
        };
        let removed = before.is_some() && self.profile.as_deref().and_then(|p| now.settings.profile(p)).is_none();
        let commit = if changed && self.mode == Mode::Henkan { self.take_shown() } else { String::new() };
        self.view = now;
        if removed {
            self.orphan = true;
        }
        self.recompute();
        commit
    }

    // ---------- what shows ----------

    fn has_staged(&self) -> bool {
        !self.prefix.is_empty() || !self.presses.is_empty()
    }

    fn entry(&self) -> Option<&Entry> {
        if self.index < 0 {
            self.negatives.get((-self.index - 1) as usize)
        } else {
            self.entries.get(self.index as usize)
        }
    }

    /// The text that shows: the prefix and the entry at the index.
    fn shown(&self) -> String {
        format!("{}{}", self.prefix, self.entry().map_or("", |e| e.text.as_str()))
    }

    /// Whether the entry that shows is a `direct` layout's.
    fn shows_direct(&self) -> bool {
        self.index >= 0 && self.entry().is_some_and(|e| e.direct)
    }

    /// What `flush` would commit now.
    fn staged(&self) -> String {
        match &self.held {
            Some(h) => h.shown().to_string(),
            None if self.has_staged() => self.shown(),
            None => String::new(),
        }
    }

    fn clear(&mut self) {
        self.prefix.clear();
        self.presses.clear();
        self.over = None;
        self.entries.clear();
        self.negatives.clear();
        self.index = 0;
        self.chosen = None;
        self.stuck = None;
    }

    fn take_shown(&mut self) -> String {
        let text = match self.held.take() {
            Some(h) => h.shown().to_string(),
            None => self.shown(),
        };
        self.clear();
        text
    }

    fn reading(&self, layout: &Layout) -> Option<Reading> {
        // A syllable table's shortcuts come from a data buffer, which this build doesn't load yet.
        read(layout, &self.presses, None)
    }

    /// Reads the staged presses with every candidate, and ranks what they say (§7.5.10).
    fn recompute(&mut self) {
        self.entries.clear();
        self.negatives.clear();
        self.index = 0;
        if self.presses.is_empty() {
            return;
        }
        let candidates = self.candidates();
        if let Some(over) = &self.over {
            let text = match over {
                Override::Keys(w) => {
                    let keys: String = self.presses.iter().filter_map(|p| jis(p.key, p.layer.shifted())).collect();
                    convert(&keys, width(*w))
                }
                Override::Script { layout, to } => candidates
                    .iter()
                    .find(|l| l.id() == layout)
                    .and_then(|l| self.reading(l))
                    .map(|r| in_script(&r.text, *to))
                    .unwrap_or_default(),
            };
            self.entries.push(Entry { text, ..Entry::default() });
            return;
        }
        let Some(profile) = self.current().cloned() else { return };
        let mut entries: Vec<Entry> = Vec::new();
        let data = self.session.data.clone();
        let dictionary = self.view.dictionary.clone();
        let script_forms = self.view.settings.script_forms(&profile);
        let evidence = self.key_evidence(&candidates);
        for (place, layout) in candidates.iter().enumerate() {
            let Some(reading) = self.reading(layout) else { continue };
            let rank = &layout.file.rank;
            let lang = layout.file.lang.as_str();
            let first = entries.len();
            // A tie goes to the layout earlier in the profile.
            let tie = -(place as f64) * 0.001;
            let row = |text: String, score: f64, note: Option<String>| Entry {
                text,
                score: score + tie,
                note,
                lang: Some(lang.to_string()),
                layout: Some(layout.id().to_string()),
                direct: layout.file.read == Read::Direct,
            };
            let text = reading.text.clone();
            let listed = data.count(&layout.file.words, &text);
            let kanji = layout.file.convert.iter().find_map(KanjiConvert::of);
            let top = match kanji.filter(|_| matches!(reading.shape, Shape::Kana | Shape::Broken)) {
                // Kana, with kanji conversion (bada ime's spec §11.2): the conversions cheapest first,
                // with the kana among them where conversion ranks it.
                Some(k) => {
                    let broken = reading.shape == Shape::Broken;
                    let kata = katakana(&text);
                    let own = dictionary.has(lang);
                    let converted = if broken {
                        None
                    } else {
                        data.convert(&k.data, &text, if own { k.user_rows } else { k.rows })
                    };
                    let conversions = converted.clone().unwrap_or_default();
                    let common = k.common.as_ref().map_or(&[][..], |(name, _)| data.common(name, &text));
                    // The user's own words that the reading writes, in whatever script: the most used first.
                    let mut mine: Vec<(&str, u32)> = Vec::new();
                    for form in [text.as_str(), kata.as_str()]
                        .into_iter()
                        .chain(conversions.iter().map(String::as_str))
                        .chain(common.iter().map(String::as_str))
                    {
                        let count = dictionary.count(lang, form);
                        if count > 0 && !mine.iter().any(|m| m.0 == form) {
                            mine.push((form, count));
                        }
                    }
                    mine.sort_by_key(|m| std::cmp::Reverse(m.1));
                    let known = listed > 0 || !mine.is_empty();
                    let score = if known {
                        rank.known
                    } else if broken {
                        rank.broken
                    } else {
                        rank.unknown
                    }
                    .unwrap_or(50.0);
                    let reading_note = || k.converted_note.then(|| text.clone());
                    let mut kana_score = score;
                    if converted.is_some() {
                        let mut at_row = 0;
                        for conv in conversions.iter().take(k.rows) {
                            let at = score + 0.5 - 0.01 * at_row as f64;
                            if *conv == text {
                                kana_score = kana_score.max(at + 0.001);
                                continue;
                            }
                            // Its katakana is a row already when script forms are on (−2).
                            if script_forms && *conv == kata {
                                continue;
                            }
                            entries.push(row(conv.clone(), at, reading_note()));
                            at_row += 1;
                        }
                        // The most common words of the reading, under the first conversion.
                        let rows = k.common.as_ref().map_or(0, |c| c.1);
                        let shown = common.iter().filter(|w| !(script_forms && **w == kata)).take(rows);
                        for (i, word) in shown.enumerate() {
                            entries.push(row(word.clone(), score + 0.5 - 0.006 - 0.0001 * i as f64, reading_note()));
                        }
                        // A known word's kana no lower than second; a word written in kana, first.
                        if known {
                            kana_score = kana_score.max(score + 0.5 - 0.005);
                        }
                        if k.native.as_ref().is_some_and(|n| data.count(std::slice::from_ref(n), &text) > 0) {
                            kana_score = kana_score.max(score + 0.6);
                        }
                    }
                    for (i, (word, _)) in mine.iter().enumerate() {
                        let at = score + 0.7 - 0.001 * i as f64;
                        if *word == text {
                            kana_score = kana_score.max(at);
                        } else {
                            entries.push(row(word.to_string(), at, reading_note()));
                        }
                    }
                    let note = k.kana_note.then(|| crate::romaji::romaji(&text)).flatten();
                    entries.push(row(text.clone(), kana_score, note));
                    kana_score
                }
                None => {
                    // A known word: in a list the layout names, or one of the user's own of its language.
                    let count = listed + dictionary.count(lang, &text);
                    let rare = reading.shape == Shape::Letters && data.is_rare(&layout.file.words, &text);
                    let score = if reading.after_text {
                        rank.after_text
                    } else if count > 0 && rank.known.is_some() {
                        rank.known.map(|known| known + if rank.by_count { f64::from(count).ln() } else { 0.0 })
                    } else if rare && rank.rare.is_some() {
                        rank.rare
                    } else {
                        match (layout.file.read, reading.shape) {
                            (Read::Search, _) => rank.query,
                            (_, Shape::Letters) => rank.letters,
                            (_, Shape::Other) => rank.other,
                            (_, Shape::Whole) => rank.whole,
                            (_, Shape::Loose) => rank.loose,
                            (_, Shape::Kana) => rank.unknown,
                            (_, Shape::Broken) => rank.broken,
                        }
                    }
                    .unwrap_or(50.0);
                    entries.push(row(text.clone(), score, None));
                    score
                }
            };
            for c in &layout.file.convert {
                if let Convert::Width { to } = c {
                    // Letters, digits and symbols in the other width; kana stay as they are.
                    let other: String =
                        text.chars().map(if *to == WidthName::Half { half_width } else { full_width }).collect();
                    if other != text {
                        entries.push(row(other, top - 0.02, None));
                    }
                }
            }
            // In a profile of several layouts the key models compare them. This layout's rows move
            // together, so its own order stays: its first reading to SET_SCORE, more for a known
            // word and for a word of the user's. The evidence itself comes after the weight.
            if evidence[place].is_some() {
                let best = entries[first..]
                    .iter()
                    .max_by(|a, b| a.score.total_cmp(&b.score))
                    .map(|e| (e.score, e.text.clone()));
                if let Some((top, shown)) = best {
                    let known = if top > 84.0 { SET_KNOWN } else { 0.0 };
                    let own = if dictionary.count(lang, &shown) > 0 { SET_OWN } else { 0.0 };
                    entries[first..].iter_mut().for_each(|e| e.score += SET_SCORE + known + own + tie - top);
                }
            }
            // The layout's weight, on everything it raised.
            let weight = self.view.settings.weight(&profile, layout.id());
            if weight != 1.0 {
                entries[first..].iter_mut().for_each(|e| e.score *= weight);
            }
            if let Some(evidence) = evidence[place] {
                entries[first..].iter_mut().for_each(|e| e.score += evidence);
            }
            // The user's negative words: a soft one far down, a strong one gone.
            let mut i = first;
            while i < entries.len() {
                match dictionary.negative(lang, &entries[i].text) {
                    Some(Strength::Strong) => {
                        entries.remove(i);
                        continue;
                    }
                    Some(Strength::Soft) => entries[i].score -= 60.0,
                    None => {}
                }
                i += 1;
            }
        }
        // Script forms, when the profile has them on: one layout's, as negative entries.
        if self.view.settings.script_forms(&profile) {
            let forms_of = |l: &Layout| {
                l.file
                    .convert
                    .iter()
                    .find_map(|c| if let Convert::Script { forms } = c { Some(forms.clone()) } else { None })
            };
            let wanted = self.view.settings.script_forms_layout(&profile);
            let from = candidates.iter().find(|l| forms_of(l).is_some() && wanted.is_none_or(|w| w == l.id()));
            if let Some((layout, reading)) = from.and_then(|l| Some((l, self.reading(l)?))) {
                let mut forms = forms_of(layout).unwrap_or_default();
                forms.sort_by_key(|f| std::cmp::Reverse(f.entry));
                self.negatives = forms
                    .iter()
                    .map(|f| Entry {
                        text: in_script(&reading.text, f.to),
                        lang: Some(layout.file.lang.clone()),
                        layout: Some(layout.id().to_string()),
                        ..Entry::default()
                    })
                    .collect();
            }
        }
        // One row per text, at its best score.
        entries.sort_by(|a, b| b.score.total_cmp(&a.score));
        let mut seen = std::collections::HashSet::new();
        entries.retain(|e| seen.insert(e.text.clone()));
        self.entries = entries;
        // After a fix the layout that was fixed goes on showing: its first row.
        if let Some(i) =
            self.stuck.as_ref().and_then(|s| self.entries.iter().position(|e| e.layout.as_ref() == Some(s)))
        {
            self.index = i as i32;
        }
    }

    /// The key models' evidence for each candidate (bada ime's spec §7.0): KEY_POINTS × ln of its
    /// share when the staged keys are scored under the model of every candidate that reads them.
    /// None for a candidate that doesn't read the keys or has no model, and for all of them unless
    /// two have one: a layout alone is ranked as it always was.
    fn key_evidence(&self, candidates: &[Arc<Layout>]) -> Vec<Option<f64>> {
        let none = vec![None; candidates.len()];
        let Some(models) = &self.session.data.key_models else { return none };
        // A press with ⌥ types the same in every layout that reads it: the models see the other keys.
        let plain = self.presses.iter().filter(|p| matches!(p.layer, Layer::Base | Layer::Shift));
        let Some(symbols) = plain.map(|&p| symbol(p)).collect::<Option<Vec<u8>>>() else { return none };
        if symbols.is_empty() {
            return none;
        }
        let scores: Vec<Option<f64>> = candidates
            .iter()
            .map(|layout| {
                let reads = self.presses.iter().all(|&p| layout.takes(p));
                // The keys as the start of a word, which ends at the last one or (counted less) goes on.
                let whole = |(keys, end): (f64, f64)| keys + (end.exp() * (1.0 - KEY_GOES_ON) + KEY_GOES_ON).ln();
                models.score(layout.id(), &symbols).filter(|_| reads).map(whole)
            })
            .collect();
        if scores.iter().flatten().count() < 2 {
            return none;
        }
        let best = scores.iter().flatten().copied().fold(f64::NEG_INFINITY, f64::max);
        let total = best + scores.iter().flatten().map(|s| (s - best).exp()).sum::<f64>().ln();
        scores.iter().map(|s| s.map(|s| (KEY_POINTS * (s - total)).max(-KEY_FLOOR))).collect()
    }

    /// Fixes what shows: the keys staged so far keep this text, whatever is typed after them, and
    /// stay staged. The layout that read them sticks.
    fn fix(&mut self) {
        if self.presses.is_empty() {
            return;
        }
        let layout = if self.index >= 0 { self.entry().and_then(|e| e.layout.clone()) } else { None };
        self.prefix = self.shown();
        self.presses.clear();
        self.chosen = None;
        self.over = None;
        self.stuck = layout;
        self.recompute();
    }

    /// Moves in the menu, without wrapping.
    fn step(&mut self, forward: bool) {
        if let Some(h) = &mut self.held {
            let n = h.variants.len();
            h.selected = Some(match (h.selected, forward) {
                (None, true) => 0,
                (None, false) => n - 1,
                (Some(i), true) => (i + 1) % n,
                (Some(i), false) => (i + n - 1) % n,
            });
            return;
        }
        self.go_to(if forward { self.index + 1 } else { self.index - 1 });
    }

    fn go_to(&mut self, index: i32) {
        let low = -(self.negatives.len() as i32);
        let high = self.entries.len() as i32 - 1;
        self.index = index.clamp(low, high.max(low));
        // A row picked below the top is remembered; back at the top, nothing is picked. Choosing a
        // row by hand ends the hold a layout has from an earlier fix.
        self.stuck = None;
        self.chosen = (self.index > 0).then(|| self.entry().map(|e| (e.text.clone(), e.layout.clone()))).flatten();
    }

    /// The reply for what shows now.
    fn finish(&self, commit: String, consumed: bool) -> Reply {
        let mut out = Reply { commit, consumed, staged: self.staged(), ..Reply::default() };
        if let Some(h) = &self.held {
            out.preedit =
                Preedit::of(h.shown(), if h.selected.is_some() { SegmentKind::Picked } else { SegmentKind::Live });
            let rows =
                h.variants.iter().enumerate().map(|(i, v)| Row { text: v.clone(), entry: i as i32, ..Row::default() });
            out.menu = Menu { rows: rows.collect(), selected: h.selected, strip: true };
            return out;
        }
        if !self.has_staged() {
            return out;
        }
        let shown = self.shown();
        let fixed = self.prefix.chars().count();
        let live = shown.chars().count() - fixed;
        let mut segments = Vec::new();
        if fixed > 0 {
            segments.push(Segment { len: fixed, kind: SegmentKind::Fixed });
        }
        if live > 0 {
            segments.push(Segment {
                len: live,
                kind: if self.index == 0 { SegmentKind::Live } else { SegmentKind::Picked },
            });
        }
        out.preedit = Preedit { caret: Some(fixed + live), text: shown, segments };
        if self.negatives.len() + self.entries.len() > 1 {
            let zero = self.negatives.len() as i32;
            let rows = self.negatives.iter().rev().chain(self.entries.iter()).enumerate().map(|(i, e)| Row {
                text: e.text.clone(),
                entry: i as i32 - zero,
                note: e.note.clone(),
                lang: e.lang.clone(),
                layout: e.layout.clone(),
            });
            out.menu = Menu { rows: rows.collect(), selected: Some((zero + self.index) as usize), strip: false };
        }
        out
    }

    fn commit_then_pass(&mut self) -> Reply {
        let commit = self.take_shown();
        self.finish(commit, false)
    }

    // ---------- events ----------

    pub fn key(&mut self, event: &KeyEvent) -> Result<Reply, Error> {
        let carried = self.sync();
        if self.orphan && self.mode == Mode::Henkan {
            return Err(Error::new(code::NO_PROFILE, "this field's profile was removed: it needs a select"));
        }
        let mut out = match self.mode {
            Mode::Muhenkan => self.key_muhenkan(event),
            Mode::Henkan => self.key_henkan(event),
        };
        out.commit.insert_str(0, &carried);
        Ok(out)
    }

    fn key_muhenkan(&mut self, e: &KeyEvent) -> Reply {
        let (key, m) = (e.key(), e.mods);
        if self.held.is_some() {
            if let Some(done) = self.key_held(e) {
                return done;
            }
        }
        // A shortcut is passed on as the physical key, whatever the layout puts there.
        if m.is_shortcut() || key_index(key).is_none() {
            return self.finish(String::new(), false);
        }
        let Some(layout) = self.muhenkan_layout() else {
            return self.finish(String::new(), false);
        };
        let press = Press { key, layer: Layer::of(m.shift, m.alt) };
        if let Some(slot) = layout.slot(press) {
            let mut out = self.finish(String::new(), false);
            out.types = Some(slot.text.clone());
            return out;
        }
        if m.alt {
            // ⌥ + a key whose character has variants: the character, its variants beside it, none chosen.
            let plain = Press { key, layer: Layer::of(m.shift, false) };
            let variants = layout
                .slot(plain)
                .and_then(|s| Some((s.text.clone(), layout.file.direct.as_ref()?.variants.get(&s.text)?.clone())));
            if let Some((letter, variants)) = variants.filter(|v| !v.1.is_empty()) {
                self.held = Some(Held { key, letter, variants, selected: None });
                return self.finish(String::new(), true);
            }
            return self.finish(String::new(), false);
        }
        // An empty slot of a typing key types nothing; Space the layout doesn't list is the app's.
        self.finish(String::new(), key.is_typing())
    }

    /// Keys while the strip of variants is open. None: the strip was closed by committing what
    /// showed, and the key goes on as usual.
    fn key_held(&mut self, e: &KeyEvent) -> Option<Reply> {
        let (key, m) = (e.key(), e.mods);
        let h = self.held.as_mut()?;
        let n = h.variants.len();
        // The same ⌥ + key again: the next variant, wrapping.
        if m.alt && !m.is_shortcut() && key == h.key {
            h.selected = Some(h.selected.map_or(0, |i| (i + 1) % n));
            return Some(self.finish(String::new(), true));
        }
        if m.is_plain() && !m.shift {
            if let Some(d) = key.code().strip_prefix("Digit").and_then(|d| d.parse::<usize>().ok()) {
                let index = if d == 0 { 9 } else { d - 1 };
                let held = self.held.take()?;
                if index < n {
                    return Some(self.finish(held.variants[index].clone(), true));
                }
                // Past the list: the letter, and the digit goes on as usual.
                let mut rest = self.key_muhenkan(e);
                rest.commit.insert_str(0, &held.letter);
                return Some(rest);
            }
        }
        match key {
            Key::ArrowRight | Key::ArrowDown => self.step(true),
            Key::ArrowLeft | Key::ArrowUp => self.step(false),
            Key::Enter => {
                let held = self.held.take()?;
                return Some(self.finish(held.shown().to_string(), true));
            }
            Key::Escape | Key::Backspace => self.held = None,
            _ => {
                // Any other key keeps what shows; then it is handled as usual.
                let held = self.held.take()?;
                let mut rest = self.key_muhenkan(e);
                rest.commit.insert_str(0, held.shown());
                return Some(rest);
            }
        }
        Some(self.finish(String::new(), true))
    }

    fn key_henkan(&mut self, e: &KeyEvent) -> Reply {
        let (key, m) = (e.key(), e.mods);
        if matches!(key, Key::F6 | Key::F7 | Key::F8 | Key::F9 | Key::F10) {
            return self.f_key(key, e);
        }
        if m.is_shortcut() {
            return self.commit_then_pass();
        }
        let staged = self.has_staged();
        let candidates = self.candidates();
        let press = Press { key, layer: Layer::of(m.shift, m.alt) };
        match key {
            Key::Tab | Key::ArrowDown | Key::ArrowUp if staged && !self.presses.is_empty() => {
                self.step(!(key == Key::ArrowUp || (key == Key::Tab && m.shift)));
                return self.finish(String::new(), true);
            }
            Key::Enter if m.shift && staged => {
                let commit = self.take_shown();
                return self.finish(commit, true);
            }
            // Space is staged only when every layout of the profile lists it (search layouts).
            Key::Space if !candidates.is_empty() && candidates.iter().all(|l| l.takes(press)) => {
                self.presses.push(press);
                self.recompute();
                return self.finish(String::new(), true);
            }
            Key::Space if m.alt => {
                let commit = format!("{}\u{3000}", self.take_shown());
                return self.finish(commit, true);
            }
            Key::Backspace if staged => {
                if self.presses.pop().is_none() {
                    self.prefix.pop();
                }
                if self.presses.is_empty() {
                    self.over = None;
                }
                self.recompute();
                return self.finish(String::new(), true);
            }
            _ => {}
        }
        if !key.is_typing() {
            return self.commit_then_pass();
        }
        // A layout whose text ends the word (§7.5.4): the character commits what shows, then is typed.
        if self.over.is_none() {
            if let Some(text) = self.ends_word(&candidates, press) {
                // Beside a direct layout whose reading shows, before the key and with it, the user
                // is typing through the key (It's, U.S.).
                if self.shows_direct() {
                    self.presses.push(press);
                    self.recompute();
                    if self.shows_direct() {
                        return self.finish(String::new(), true);
                    }
                    self.presses.pop();
                    self.recompute();
                }
                let mut commit = if self.has_staged() { self.take_shown() } else { String::new() };
                commit.push_str(&text);
                return self.finish(commit, true);
            }
        }
        if self.over.is_some() || candidates.iter().any(|l| l.takes(press)) {
            // A row picked in the menu is fixed by typing on: the new key starts after it.
            if self.chosen.is_some() {
                self.fix();
            }
            self.presses.push(press);
            self.recompute();
            return self.finish(String::new(), true);
        }
        // No layout has a slot for it. An empty slot types nothing; ⌥ + a key is the app's.
        let commit = self.take_shown();
        self.finish(commit, !m.alt)
    }

    /// The character a press types as it is, in a layout of the profile whose text ends the word;
    /// None when the press is part of a word, or when another layout reads the key as more than text.
    fn ends_word(&self, candidates: &[Arc<Layout>], press: Press) -> Option<String> {
        let layout = candidates.iter().find(|l| l.file.text_ends_word)?;
        let slot = layout.slot(press)?;
        let staged: Vec<_> = self.presses.iter().filter_map(|&p| layout.slot(p)).collect();
        let text = match &layout.file.table {
            Some(Table::Syllable { syllable }) => is_text(syllable, &staged, slot),
            _ => slot.role == Role::Text,
        };
        let others = candidates.iter().any(|l| {
            !Arc::ptr_eq(l, layout)
                && l.file.read != Read::Direct
                && l.slot(press).is_some_and(|s| s.role != Role::Text)
        });
        (text && !others).then(|| slot.text.clone())
    }

    /// F6–F10: the profile's own (§8.1).
    fn f_key(&mut self, key: Key, e: &KeyEvent) -> Reply {
        let fkeys = self.current().and_then(|p| p.fkeys.clone()).unwrap_or_default();
        if !fkeys.intercept {
            return self.finish(String::new(), false);
        }
        let binding = fkeys.keys.get(key.code()).cloned().unwrap_or_else(|| "swallow".into());
        if binding == "pass" {
            return self.finish(String::new(), false);
        }
        let m = e.mods;
        if binding == "swallow" || m.shift || m.alt || m.ctrl || m.meta {
            return self.finish(String::new(), true);
        }
        // An action only acts for a layout among the profile's candidates; else the key is swallowed.
        self.layout_action(&binding).unwrap_or_else(|_| self.finish(String::new(), true))
    }

    pub fn select(&mut self, mode: Mode, profile: Option<&str>) -> Result<Reply, Error> {
        let carried = self.sync();
        if let Some(id) = profile {
            if self.view.settings.profile(id).is_none() {
                if mode == Mode::Henkan && self.view.settings.henkan.profiles.is_empty() {
                    return Err(Error::new(code::NO_PROFILE, "the settings have no henkan profile"));
                }
                return Err(Error::at(code::UNKNOWN_PROFILE, "profile", format!("no profile named {id}")));
            }
        } else if mode == Mode::Henkan {
            return Err(Error::new(code::NO_PROFILE, "henkan needs a profile"));
        }
        let mut commit = carried;
        if mode != self.mode || profile != self.profile.as_deref() {
            commit.push_str(&self.take_shown());
            self.mode = mode;
            self.profile = profile.map(str::to_string);
            self.waiting = None;
        }
        self.orphan = false;
        Ok(self.finish(commit, false))
    }

    pub fn flush(&mut self, _cause: Cause) -> Reply {
        let mut commit = self.sync();
        commit.push_str(&self.staged());
        self.held = None;
        self.clear();
        self.finish(commit, false)
    }

    pub fn reset(&mut self) -> Reply {
        self.view = self.session.view();
        self.held = None;
        self.waiting = None;
        self.clear();
        self.finish(String::new(), false)
    }

    pub fn action(&mut self, name: &str) -> Result<Reply, Error> {
        let carried = self.sync();
        let mut out = match name {
            "menu.next" | "menu.prev" => {
                if self.held.is_some() || !self.presses.is_empty() {
                    self.step(name == "menu.next");
                }
                self.finish(String::new(), true)
            }
            "fix" => {
                if self.mode == Mode::Henkan {
                    self.fix();
                }
                self.finish(String::new(), true)
            }
            "width.full" | "width.half" => {
                let w = if name == "width.full" { Width::Full } else { Width::Half };
                if self.mode == Mode::Henkan && self.has_staged() {
                    self.prefix = convert(&self.shown(), w);
                    self.presses.clear();
                    self.over = None;
                    self.recompute();
                    self.finish(String::new(), true)
                } else {
                    self.waiting = Some(Waiting::Width(w));
                    let mut out = self.finish(String::new(), true);
                    out.ask = Some(Ask::Selection(if w == Width::Full { "full" } else { "half" }.into()));
                    out
                }
            }
            other => self.layout_action(other)?,
        };
        out.commit.insert_str(0, &carried);
        Ok(out)
    }

    /// `<layout id>.<action id>`: one of the engine's general operations, as the layout's file names it.
    fn layout_action(&mut self, name: &str) -> Result<Reply, Error> {
        let unknown = || {
            Error::new(
                code::UNKNOWN_ACTION,
                format!("no action named {name}, or its layout isn't in the profile in use"),
            )
        };
        let (layout_id, action_id) = name.split_once('.').ok_or_else(unknown)?;
        let layout = self.candidates().into_iter().find(|l| l.id() == layout_id).ok_or_else(unknown)?;
        let what = layout.action(action_id).ok_or_else(unknown)?.what.clone();
        let mut out = None;
        match what {
            Do::Keys { width } if !self.presses.is_empty() => {
                self.over = Some(Override::Keys(width));
                self.recompute();
            }
            Do::Script { to } if !self.presses.is_empty() => {
                self.over = Some(Override::Script { layout: layout_id.to_string(), to });
                self.recompute();
            }
            Do::Jamo { keep } => {
                if self.has_staged() {
                    self.prefix = keep_jamo(&self.shown(), keep);
                    self.presses.clear();
                    self.over = None;
                    self.recompute();
                } else {
                    // Staged text comes before the selection: only now is the selection asked for.
                    self.waiting = Some(Waiting::Jamo(keep));
                    let mut asked = self.finish(String::new(), true);
                    asked.ask = Some(Ask::Selection(action_id.to_string()));
                    out = Some(asked);
                }
            }
            // A lookup needs its data buffer, which this build doesn't load yet: nothing to list.
            Do::Lookup { .. } | Do::Keys { .. } | Do::Script { .. } => {}
        }
        Ok(out.unwrap_or_else(|| self.finish(String::new(), true)))
    }

    pub fn pick(&mut self, row: usize) -> Result<Reply, Error> {
        let carried = self.sync();
        let bad = || Error::at(code::BAD_ROW, "row", format!("the menu has no row {row}"));
        if let Some(h) = &mut self.held {
            if row >= h.variants.len() {
                return Err(bad());
            }
            h.selected = Some(row);
        } else {
            let rows = self.negatives.len() + self.entries.len();
            if self.presses.is_empty() || rows <= 1 || row >= rows {
                return Err(bad());
            }
            self.go_to(row as i32 - self.negatives.len() as i32);
        }
        Ok(self.finish(carried, true))
    }

    pub fn text(&mut self, text: &Text) -> Reply {
        let carried = self.sync();
        let waiting = self.waiting.take();
        let selected = text.selection.as_deref().unwrap_or("");
        let made = match waiting {
            Some(Waiting::Width(w)) if !selected.is_empty() => Some(convert(selected, w)),
            Some(Waiting::Jamo(keep)) if !selected.is_empty() => Some(keep_jamo(selected, keep)),
            _ => None,
        };
        let mut out = self.finish(carried, true);
        if let Some(made) = made {
            out.commit.push_str(&made);
            out.replace = Replace::Selection;
        }
        out
    }
}
