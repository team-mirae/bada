//! Layout files (`bada-layout/2`, the standard §7.5): the file as written, the checks that refuse
//! one, and the form the readers use. **No layout has code of its own**: everything a layout is
//! comes from here.

use std::collections::BTreeMap;

use bada_compat::{ActionInfo, Key, LAYOUT_FORMAT, LayoutInfo, Mode, Problem, Texts};
use serde::{Deserialize, Deserializer, Serialize};
use serde_json::Value;

/// The typing keys and Space: the keys a layout can have slots for.
pub const KEYS: usize = 49;

/// The JIS table (§7.2): what a typing key types where a layout doesn't say. ⇧0 is empty.
pub fn jis(key: Key, shift: bool) -> Option<char> {
    use Key::*;
    if let Some(letter) = key.letter() {
        return Some(if shift { letter.to_ascii_uppercase() } else { letter });
    }
    let (base, shifted) = match key {
        Digit1 => ('1', Some('!')),
        Digit2 => ('2', Some('"')),
        Digit3 => ('3', Some('#')),
        Digit4 => ('4', Some('$')),
        Digit5 => ('5', Some('%')),
        Digit6 => ('6', Some('&')),
        Digit7 => ('7', Some('\'')),
        Digit8 => ('8', Some('(')),
        Digit9 => ('9', Some(')')),
        Digit0 => ('0', None),
        Minus => ('-', Some('=')),
        Equal => ('^', Some('~')),
        IntlYen => ('¥', Some('|')),
        BracketLeft => ('@', Some('`')),
        BracketRight => ('[', Some('{')),
        Semicolon => (';', Some('+')),
        Quote => (':', Some('*')),
        Backslash => (']', Some('}')),
        Comma => (',', Some('<')),
        Period => ('.', Some('>')),
        Slash => ('/', Some('?')),
        IntlRo => ('\\', Some('_')),
        _ => return None,
    };
    if shift { shifted } else { Some(base) }
}

/// A key's layers: no modifier, ⇧, ⌥ and ⌥⇧. ⌃ and ⌘ are never a layer.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Layer {
    Base,
    Shift,
    Alt,
    AltShift,
}

impl Layer {
    pub fn of(shift: bool, alt: bool) -> Layer {
        match (shift, alt) {
            (false, false) => Layer::Base,
            (true, false) => Layer::Shift,
            (false, true) => Layer::Alt,
            (true, true) => Layer::AltShift,
        }
    }

    pub fn shifted(self) -> bool {
        matches!(self, Layer::Shift | Layer::AltShift)
    }
}

/// One staged press: a typing key or Space, on a layer.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Press {
    pub key: Key,
    pub layer: Layer,
}

/// What a slot is to its way of reading (§7.5.1).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum Role {
    /// Text, typed as it is.
    #[serde(rename = "x")]
    Text,
    /// A letter: of a word, of a spelling, of the search.
    #[serde(rename = "l")]
    Letter,
    /// A consonant, both an initial and a final.
    #[serde(rename = "c")]
    Consonant,
    #[serde(rename = "i")]
    Initial,
    #[serde(rename = "f")]
    Final,
    #[serde(rename = "v")]
    Vowel,
    /// A break between syllables.
    #[serde(rename = "b")]
    Break,
    #[serde(rename = "k")]
    Kana,
    /// A mark that attaches to the kana before it.
    #[serde(rename = "m")]
    Mark,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Slot {
    /// What the key types, or its letter, jamo or kana.
    #[serde(rename = "t")]
    pub text: String,
    #[serde(rename = "r")]
    pub role: Role,
}

/// A slot as written: left out, `null`, or there.
fn written<'de, D: Deserializer<'de>>(d: D) -> Result<Option<Option<Slot>>, D::Error> {
    Option::<Slot>::deserialize(d).map(Some)
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct KeyFile {
    #[serde(default, deserialize_with = "written", skip_serializing_if = "Option::is_none")]
    pub base: Option<Option<Slot>>,
    #[serde(default, deserialize_with = "written", skip_serializing_if = "Option::is_none")]
    pub shift: Option<Option<Slot>>,
    #[serde(default, deserialize_with = "written", skip_serializing_if = "Option::is_none")]
    pub alt: Option<Option<Slot>>,
    #[serde(default, deserialize_with = "written", skip_serializing_if = "Option::is_none")]
    pub alt_shift: Option<Option<Slot>>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Read {
    Direct,
    Hangul,
    Table,
    Kana,
    Search,
}

impl Read {
    pub fn name(self) -> &'static str {
        match self {
            Read::Direct => "direct",
            Read::Hangul => "hangul",
            Read::Table => "table",
            Read::Kana => "kana",
            Read::Search => "search",
        }
    }

    fn roles(self) -> &'static [Role] {
        use Role::*;
        match self {
            Read::Direct | Read::Search => &[Letter, Text],
            Read::Hangul => &[Consonant, Initial, Final, Vowel, Text],
            Read::Table => &[Letter, Break, Text],
            Read::Kana => &[Kana, Mark, Text],
        }
    }

    fn has_letters(self) -> bool {
        matches!(self, Read::Direct | Read::Table | Read::Search)
    }
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Direct {
    /// A character's variants, offered in a strip when its key is pressed with ⌥ in muhenkan.
    pub variants: BTreeMap<String, Vec<String>>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Hangul {
    pub vowels: BTreeMap<String, String>,
    pub finals: BTreeMap<String, String>,
    pub double_tense: bool,
    pub final_moves_on: bool,
    #[serde(default)]
    pub implied_ieung: bool,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rule {
    #[serde(rename = "in")]
    pub letters: String,
    pub out: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub before: Option<String>,
    #[serde(default)]
    pub keep: usize,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CodaBefore {
    pub coda: String,
    pub next: String,
    pub jamo: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Shortcuts {
    pub data: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub option: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Syllable {
    pub onsets: BTreeMap<String, Vec<String>>,
    pub vowels: BTreeMap<String, Vec<String>>,
    pub codas: BTreeMap<String, Vec<String>>,
    #[serde(default)]
    pub empty_onset: Vec<String>,
    #[serde(default)]
    pub vowels_before_coda: BTreeMap<String, Vec<String>>,
    #[serde(default)]
    pub codas_before: Vec<CodaBefore>,
    #[serde(default)]
    pub letter_after: BTreeMap<String, Vec<String>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub shortcuts: Option<Shortcuts>,
}

/// The `table` part, in one of its two forms.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(untagged, deny_unknown_fields)]
pub enum Table {
    Spellings {
        spellings: BTreeMap<String, String>,
        #[serde(default)]
        rules: Vec<Rule>,
    },
    Syllable {
        syllable: Syllable,
    },
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Kana {
    /// Each mark, and what it makes of the kana before it.
    pub marks: BTreeMap<String, BTreeMap<String, String>>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Source {
    pub data: String,
    #[serde(rename = "match")]
    pub columns: Vec<String>,
    pub tiers: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub order: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub note: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub single: Option<String>,
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub forms: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tones: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Code {
    pub max_digits: usize,
    #[serde(default)]
    pub after: Vec<String>,
    #[serde(default)]
    pub digit_first_after: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Search {
    pub sources: Vec<Source>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub code: Option<Code>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub min_length: Option<usize>,
    pub limit: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Script {
    Hiragana,
    Katakana,
    HalfwidthKatakana,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum WidthName {
    Full,
    Half,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Form {
    pub entry: i32,
    pub to: Script,
}

/// What turns the reading into more rows (§7.5.7).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "with", rename_all = "lowercase", deny_unknown_fields)]
pub enum Convert {
    Kanji {
        data: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        rows: Option<usize>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        user_rows: Option<usize>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        common: Option<Value>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        native: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        notes: Option<Value>,
    },
    Script {
        forms: Vec<Form>,
    },
    Width {
        to: WidthName,
    },
    Dictionary {
        data: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        rows: Option<usize>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        first: Option<bool>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        below_top: Option<f64>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        case: Option<Value>,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Keep {
    Initials,
    Vowels,
    Finals,
}

/// One of the engine's general operations, with its argument (§7.5.8).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "do", rename_all = "lowercase")]
pub enum Do {
    Script { to: Script },
    Keys { width: WidthName },
    Jamo { keep: Keep },
    Lookup { data: String },
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Action {
    pub id: String,
    pub label: Texts,
    #[serde(flatten)]
    pub what: Do,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Stack {
    pub first: Vec<String>,
    pub max: usize,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Particles {
    pub after: Vec<String>,
    pub words: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub stack: Option<Stack>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Readings {
    pub cost: f64,
    pub max_cost: f64,
    pub floor: f64,
    pub rows: usize,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Model {
    pub data: String,
    pub cost: f64,
    pub margin: f64,
}

/// The scores of a layout's readings (§7.5.10).
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Rank {
    pub known: Option<f64>,
    pub by_count: bool,
    pub rare: Option<f64>,
    pub letters: Option<f64>,
    pub other: Option<f64>,
    pub whole: Option<f64>,
    pub loose: Option<f64>,
    pub unknown: Option<f64>,
    pub broken: Option<f64>,
    pub after_text: Option<f64>,
    pub query: Option<f64>,
    pub found: Option<f64>,
    pub readings: Option<Readings>,
    pub model: Option<Model>,
}

/// A layout file, as written.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct File {
    pub format: String,
    pub id: String,
    pub name: Texts,
    pub lang: String,
    pub kind: Mode,
    pub read: Read,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub note: Option<String>,
    pub keys: BTreeMap<String, KeyFile>,
    #[serde(default)]
    pub text_ends_word: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub direct: Option<Direct>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub hangul: Option<Hangul>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub table: Option<Table>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kana: Option<Kana>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub search: Option<Search>,
    #[serde(default)]
    pub convert: Vec<Convert>,
    #[serde(default)]
    pub actions: Vec<Action>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub particles: Option<Particles>,
    #[serde(default)]
    pub words: Vec<String>,
    #[serde(default)]
    pub rank: Rank,
    #[serde(default)]
    pub options: BTreeMap<String, Value>,
}

/// A layout, checked and ready to read keys with.
#[derive(Clone, Debug)]
pub struct Layout {
    pub file: File,
    /// Every key's four layers, with the JIS table under the layout's own slots.
    slots: Vec<[Option<Slot>; 4]>,
    /// One of the layouts shipped with the engine; and whether a custom one replaces it.
    pub stock: bool,
    pub modified: bool,
}

/// The index of a typing key or Space among a layout's keys.
pub fn key_index(key: Key) -> Option<usize> {
    let i = key as usize;
    (i < KEYS).then_some(i)
}

fn id_is_a_slug(id: &str) -> bool {
    let mut parts = id.split('-');
    let group = parts.next().unwrap_or("");
    let rest: Vec<&str> = parts.collect();
    (2..=3).contains(&group.len())
        && group.bytes().all(|b| b.is_ascii_lowercase())
        && !rest.is_empty()
        && rest.iter().all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit()))
        && id.len() <= 48
}

impl Layout {
    /// Reads and checks a layout object. Error-level problems: the layout isn't loaded (§7.6).
    pub fn load(value: &Value, stock: bool) -> Result<Layout, Vec<Problem>> {
        let one = |path: &str, code: &str, message: String| vec![Problem::error(path, code, message)];
        if value.get("format").and_then(Value::as_str) != Some(LAYOUT_FORMAT) {
            return Err(one("format", "bad-format", format!("a layout file's format is {LAYOUT_FORMAT}")));
        }
        if value.get("id").and_then(Value::as_str).is_none() {
            return Err(one("id", "missing-id", "a layout has an id".into()));
        }
        let file: File = serde_json::from_value(value.clone()).map_err(|e| {
            let message = e.to_string();
            let code = if message.starts_with("unknown field") { "unknown-field" } else { "bad-value" };
            one("", code, message)
        })?;
        let mut problems = Vec::new();
        let mut error = |path: &str, code: &str, message: String| problems.push(Problem::error(path, code, message));
        if !id_is_a_slug(&file.id) {
            error("id", "bad-value", "an id is <group>-<name>: lowercase letters, digits and hyphens".into());
        }
        if !file.name.contains_key("en") {
            error("name", "bad-value", "a name has en".into());
        }
        if (file.kind == Mode::Muhenkan) != (file.read == Read::Direct) {
            error("kind", "contradiction", "kind is muhenkan exactly when read is direct".into());
        }
        let parts = [
            (Read::Direct, file.direct.is_some()),
            (Read::Hangul, file.hangul.is_some()),
            (Read::Table, file.table.is_some()),
            (Read::Kana, file.kana.is_some()),
            (Read::Search, file.search.is_some()),
        ];
        for (read, there) in parts {
            if there && read != file.read {
                error(read.name(), "contradiction", format!("{} is for read {}", read.name(), read.name()));
            }
            if !there && read == file.read && read != Read::Direct {
                error(read.name(), "contradiction", format!("read {} needs its {} part", read.name(), read.name()));
            }
        }
        let mut slots: Vec<[Option<Slot>; 4]> = (0..KEYS)
            .map(|i| {
                let key = Key::ALL[i];
                let default = |shift| {
                    jis(key, shift).map(|c| Slot {
                        text: c.to_string(),
                        role: if key.letter().is_some() && file.read.has_letters() { Role::Letter } else { Role::Text },
                    })
                };
                [default(false), default(true), None, None]
            })
            .collect();
        for (code, key) in &file.keys {
            let Some(i) = key_index(Key::from_code(code)) else {
                error(&format!("keys.{code}"), "unknown-key", format!("{code} isn't a typing key or Space"));
                continue;
            };
            for (layer, slot) in [&key.base, &key.shift, &key.alt, &key.alt_shift].into_iter().enumerate() {
                let Some(slot) = slot else { continue };
                if let Some(s) = slot {
                    if !file.read.roles().contains(&s.role) {
                        let name = ["base", "shift", "alt", "alt_shift"][layer];
                        error(
                            &format!("keys.{code}.{name}.r"),
                            "bad-role",
                            format!("read {} has no such role", file.read.name()),
                        );
                    }
                    if s.text.is_empty() {
                        error(&format!("keys.{code}"), "bad-value", "a slot types something, or is null".into());
                    }
                }
                slots[i][layer] = slot.clone();
            }
        }
        // Every option a table uses is one the layout declares.
        let mut used: Vec<&str> = Vec::new();
        if let Some(Table::Syllable { syllable }) = &file.table {
            used.extend(syllable.shortcuts.as_ref().and_then(|s| s.option.as_deref()));
        }
        if let Some(search) = &file.search {
            used.extend(search.sources.iter().filter_map(|s| s.tones.as_deref()));
        }
        for option in used {
            if !file.options.contains_key(option) {
                error("options", "contradiction", format!("the option {option} is used but not declared"));
            }
        }
        if let Some(Table::Spellings { spellings, rules }) = &file.table {
            if spellings.keys().any(String::is_empty) || rules.iter().any(|r| r.letters.is_empty()) {
                error("table", "contradiction", "a spelling has letters".into());
            }
            if let Some(r) = rules.iter().find(|r| r.keep >= r.letters.chars().count()) {
                error(
                    "table.rules",
                    "contradiction",
                    format!("the rule {} keeps all of its letters: it would never end", r.letters),
                );
            }
        }
        if problems.is_empty() { Ok(Layout { file, slots, stock, modified: false }) } else { Err(problems) }
    }

    pub fn id(&self) -> &str {
        &self.file.id
    }

    /// What a press types in this layout, if the layout has a slot for it.
    pub fn slot(&self, press: Press) -> Option<&Slot> {
        self.slots[key_index(press.key)?][press.layer as usize].as_ref()
    }

    pub fn takes(&self, press: Press) -> bool {
        self.slot(press).is_some()
    }

    pub fn action(&self, id: &str) -> Option<&Action> {
        self.file.actions.iter().find(|a| a.id == id)
    }

    /// The layout as lists show it.
    pub fn info(&self) -> LayoutInfo {
        LayoutInfo {
            id: self.file.id.clone(),
            name: self.file.name.clone(),
            kind: self.file.kind,
            read: Some(self.file.read.name().into()),
            lang: Some(self.file.lang.clone()),
            actions: self
                .file
                .actions
                .iter()
                .map(|a| ActionInfo { id: a.id.clone(), label: a.label.clone() })
                .collect(),
            options: serde_json::to_value(&self.file.options).unwrap_or_default(),
            stock: self.stock,
            modified: self.modified,
        }
    }
}

/// The eleven layouts shipped with the engine: files like any other.
pub const STOCK: [(&str, &str); 11] = [
    ("en-qwerty", include_str!("../layouts/en-qwerty.json")),
    ("en-qwerty-us", include_str!("../layouts/en-qwerty-us.json")),
    ("en-dvorak", include_str!("../layouts/en-dvorak.json")),
    ("en-colemak", include_str!("../layouts/en-colemak.json")),
    ("ko-2-set", include_str!("../layouts/ko-2-set.json")),
    ("ko-bada-roman", include_str!("../layouts/ko-bada-roman.json")),
    ("ja-romaji", include_str!("../layouts/ja-romaji.json")),
    ("ja-kana", include_str!("../layouts/ja-kana.json")),
    ("uni-unicode", include_str!("../layouts/uni-unicode.json")),
    ("uni-symbols", include_str!("../layouts/uni-symbols.json")),
    ("uni-emojis", include_str!("../layouts/uni-emojis.json")),
];

/// The stock layouts, read once.
pub fn stock() -> &'static [std::sync::Arc<Layout>] {
    static LAYOUTS: std::sync::OnceLock<Vec<std::sync::Arc<Layout>>> = std::sync::OnceLock::new();
    LAYOUTS.get_or_init(|| {
        STOCK
            .iter()
            .map(|(id, text)| {
                let value: Value = serde_json::from_str(text).unwrap_or_else(|e| panic!("{id}.json isn't JSON: {e}"));
                let layout = Layout::load(&value, true).unwrap_or_else(|p| panic!("{id}.json: {p:?}"));
                assert_eq!(layout.id(), *id, "a stock file is named by its id");
                std::sync::Arc::new(layout)
            })
            .collect()
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn press(key: Key, layer: Layer) -> Press {
        Press { key, layer }
    }

    #[test]
    fn the_stock_eleven_load() {
        assert_eq!(stock().len(), 11);
        for layout in stock() {
            assert!(layout.stock && !layout.modified);
            assert_eq!(layout.file.kind == Mode::Muhenkan, layout.file.read == Read::Direct, "{}", layout.id());
        }
    }

    #[test]
    fn a_slot_left_out_is_the_jis_tables() {
        let find = |id: &str| stock().iter().find(|l| l.id() == id).unwrap().clone();
        let qwerty = find("en-qwerty");
        assert_eq!(qwerty.slot(press(Key::IntlRo, Layer::Base)).unwrap().text, "\\");
        assert_eq!(qwerty.slot(press(Key::IntlRo, Layer::Shift)).unwrap().text, "_");
        assert_eq!(qwerty.slot(press(Key::KeyA, Layer::Shift)).unwrap().role, Role::Letter);
        assert!(qwerty.slot(press(Key::Digit0, Layer::Shift)).is_none(), "⇧0 is empty");
        assert!(qwerty.slot(press(Key::KeyA, Layer::Alt)).is_none(), "⌥ is the layout's only where listed");
        assert_eq!(qwerty.slot(press(Key::Space, Layer::Alt)).unwrap().text, "_");
        assert!(qwerty.slot(press(Key::Space, Layer::Base)).is_none());
        // The Korean layouts type ₩ where ¥ is.
        for id in ["ko-2-set", "ko-bada-roman"] {
            assert_eq!(find(id).slot(press(Key::IntlYen, Layer::Base)).unwrap().text, "₩", "{id}");
            assert_eq!(find(id).slot(press(Key::IntlYen, Layer::Shift)).unwrap().text, "|", "{id}");
        }
        assert_eq!(find("en-dvorak").slot(press(Key::Digit0, Layer::Shift)).unwrap().text, "~");
        assert_eq!(find("ko-2-set").slot(press(Key::KeyA, Layer::Base)).unwrap().role, Role::Consonant);
        assert_eq!(find("ko-2-set").slot(press(Key::Digit1, Layer::Base)).unwrap().role, Role::Text);
        assert_eq!(find("ja-kana").slot(press(Key::Slash, Layer::AltShift)).unwrap().text, "？");
        assert_eq!(find("uni-symbols").slot(press(Key::Space, Layer::Base)).unwrap().text, " ");
    }

    #[test]
    fn what_is_wrong_with_a_layout_is_a_problem() {
        let good: Value = serde_json::from_str(STOCK[0].1).unwrap();
        let with = |change: &dyn Fn(&mut Value)| {
            let mut v = good.clone();
            change(&mut v);
            Layout::load(&v, false).err().map(|p| p[0].code.clone())
        };
        assert_eq!(with(&|_| {}), None);
        assert_eq!(with(&|v| v["format"] = "bada-layout/1".into()), Some("bad-format".into()));
        assert_eq!(with(&|v| v["id"] = 15.into()), Some("missing-id".into()));
        assert_eq!(with(&|v| v["id"] = "Dvorak".into()), Some("bad-value".into()));
        assert_eq!(with(&|v| v["input"] = "direct".into()), Some("unknown-field".into()));
        assert_eq!(with(&|v| v["kind"] = "henkan".into()), Some("contradiction".into()));
        assert_eq!(
            with(&|v| v["keys"]["Enter"] = serde_json::json!({"base": {"t": "x", "r": "x"}})),
            Some("unknown-key".into())
        );
        assert_eq!(
            with(&|v| v["keys"]["KeyA"] = serde_json::json!({"base": {"t": "ㅁ", "r": "c"}})),
            Some("bad-role".into())
        );
        assert_eq!(
            with(&|v| v["hangul"] =
                serde_json::json!({"vowels": {}, "finals": {}, "double_tense": false, "final_moves_on": true})),
            Some("contradiction".into())
        );
    }
}
