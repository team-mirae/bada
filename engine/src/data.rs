//! The engine's data (the standard, §11): named buffers the harness passes once, shared by every
//! session. A buffer the engine wasn't given turns its feature off. Also the user dictionary, which
//! is the session's.

use std::collections::{BTreeMap, HashMap};

use bada_compat::{Buffer, DictionarySummary, Problem};
use serde::Deserialize;
use serde_json::Value;

use crate::kanji::Kanji;
use crate::keymodel::KeyModels;
use crate::search::Table;

/// A word list, packed: the words sorted in one string, with offsets and counts. Under 30 bytes a
/// word, so a million words fit in some 30 MB.
///
/// The buffer is lines of `word` or `word<TAB>count`; `#` starts a comment, and after a line
/// `#! rare` come words that are known, but not common. Words are matched in lower case.
#[derive(Default)]
pub struct WordList {
    text: String,
    /// Word `i` is `text[starts[i]..starts[i + 1]]`.
    starts: Vec<u32>,
    counts: Vec<u32>,
    rare: Vec<Box<str>>,
}

impl WordList {
    pub fn parse(text: &str) -> WordList {
        let mut words: Vec<(String, u32)> = Vec::new();
        let mut list = WordList::default();
        let mut rare = false;
        let mut sorted = true;
        for line in text.lines() {
            if line.starts_with("#! rare") {
                rare = true;
                continue;
            }
            let mut fields = line.split('\t');
            let word = fields.next().unwrap_or("").trim();
            if word.is_empty() || word.starts_with('#') {
                continue;
            }
            let word = word.to_lowercase();
            if rare {
                list.rare.push(word.into_boxed_str());
                continue;
            }
            let count = fields.next().and_then(|c| c.trim().parse::<u32>().ok()).unwrap_or(1).max(1);
            sorted &= words.last().is_none_or(|last| last.0 < word);
            words.push((word, count));
        }
        if !sorted {
            words.sort_unstable_by(|a, b| a.0.cmp(&b.0));
            words.dedup_by(|b, a| a.0 == b.0);
        }
        list.text.reserve(words.iter().map(|w| w.0.len()).sum());
        for (word, count) in &words {
            list.starts.push(list.text.len() as u32);
            list.text.push_str(word);
            list.counts.push(*count);
        }
        list.starts.push(list.text.len() as u32);
        list.rare.sort_unstable();
        list
    }

    fn word(&self, i: usize) -> &str {
        &self.text[self.starts[i] as usize..self.starts[i + 1] as usize]
    }

    /// How often the word was seen: 0 if it isn't in the list, at least 1 if it is.
    pub fn count(&self, word: &str) -> u32 {
        let word = word.to_lowercase();
        let (mut lo, mut hi) = (0, self.counts.len());
        while lo < hi {
            let mid = (lo + hi) / 2;
            match self.word(mid).cmp(word.as_str()) {
                std::cmp::Ordering::Less => lo = mid + 1,
                std::cmp::Ordering::Greater => hi = mid,
                std::cmp::Ordering::Equal => return self.counts[mid],
            }
        }
        0
    }

    /// Whether the word is in the list's rare section.
    pub fn is_rare(&self, word: &str) -> bool {
        let word = word.to_lowercase();
        self.rare.binary_search_by(|w| (**w).cmp(word.as_str())).is_ok()
    }

    pub fn len(&self) -> usize {
        self.counts.len()
    }

    pub fn is_empty(&self) -> bool {
        self.counts.is_empty() && self.rare.is_empty()
    }
}

/// The buffers bada knows, and whether it can use each in place when it is lent (`P_BORROWED`).
pub const BUFFERS: [(&str, bool); 14] = [
    ("ko-words", false),
    ("en-words", false),
    ("ja-words", false),
    ("ja-kana-native", false),
    ("ja-common-words", false),
    ("kanji", true),
    ("hanja", false),
    ("ko-jamo-4gram", true),
    ("ja-shortcuts", false),
    ("en-ko", false),
    ("symbols", false),
    ("emoji", false),
    ("unicode-names", false),
    ("key-models", true),
];

/// The word lists among them: a layout's `words` names these.
const WORD_LISTS: [&str; 4] = ["ko-words", "en-words", "ja-words", "ja-kana-native"];

#[derive(Default)]
pub struct Data {
    words: BTreeMap<String, WordList>,
    /// Kana-kanji conversion, by buffer name.
    kanji: BTreeMap<String, Kanji>,
    /// The key models that compare the layouts of a profile.
    pub key_models: Option<KeyModels>,
    /// Reading → the common words read so, most common first, by buffer name.
    common: BTreeMap<String, HashMap<String, Vec<String>>>,
    /// The tables a `search` layout looks things up in, by buffer name.
    pub tables: BTreeMap<String, Table>,
    /// The buffers no reader uses yet, kept as they came.
    pub raw: BTreeMap<String, Vec<u8>>,
}

impl Data {
    pub fn new(buffers: &[Buffer<'_>]) -> Data {
        let mut data = Data::default();
        for buffer in buffers.iter().filter(|b| BUFFERS.iter().any(|k| k.0 == b.name)) {
            let text = || String::from_utf8_lossy(buffer.bytes);
            match buffer.name {
                name if WORD_LISTS.contains(&name) => {
                    data.words.insert(name.to_string(), WordList::parse(&text()));
                }
                "kanji" => {
                    // A malformed file is ignored: no kanji.
                    if let Ok(kanji) = Kanji::parse(buffer.bytes) {
                        data.kanji.insert(buffer.name.to_string(), kanji);
                    }
                }
                "key-models" => data.key_models = KeyModels::parse(buffer.bytes),
                "ja-common-words" => {
                    let mut by_reading: HashMap<String, Vec<String>> = HashMap::new();
                    for line in text().lines().filter(|l| !l.starts_with('#')) {
                        let mut fields = line.split('\t');
                        if let (Some(word), Some(reading)) = (fields.next(), fields.next()) {
                            by_reading.entry(reading.to_string()).or_default().push(word.to_string());
                        }
                    }
                    data.common.insert(buffer.name.to_string(), by_reading);
                }
                name => match Table::parse(name, &text()) {
                    Some(table) => {
                        data.tables.insert(name.to_string(), table);
                    }
                    None => {
                        data.raw.insert(name.to_string(), buffer.bytes.to_vec());
                    }
                },
            }
        }
        // Words usually written in kana convert to kana (この, あなた).
        if let (Some(kanji), Some(native)) = (data.kanji.get_mut("kanji"), data.words.get("ja-kana-native")) {
            let words: Vec<&str> = (0..native.len()).map(|i| native.word(i)).collect();
            kanji.prefer_kana(words);
        }
        data
    }

    /// The conversions of a kana reading, cheapest first, at most `n`. None without the buffer.
    pub fn convert(&self, name: &str, hiragana: &str, n: usize) -> Option<Vec<String>> {
        Some(self.kanji.get(name)?.convert(hiragana, n).into_iter().map(|c| c.0).collect())
    }

    /// The common words read `reading`, most common first.
    pub fn common(&self, name: &str, reading: &str) -> &[String] {
        self.common.get(name).and_then(|m| m.get(reading)).map_or(&[], Vec::as_slice)
    }

    pub fn has(&self, name: &str) -> bool {
        self.words.contains_key(name)
            || self.raw.contains_key(name)
            || self.tables.contains_key(name)
            || self.kanji.contains_key(name)
            || self.common.contains_key(name)
    }

    /// How often a reading was seen in the lists a layout names: 0 if it is in none.
    pub fn count(&self, lists: &[String], word: &str) -> u32 {
        lists.iter().filter_map(|name| self.words.get(name)).map(|list| list.count(word)).max().unwrap_or(0)
    }

    /// Whether a reading is in the rare section of a list a layout names.
    pub fn is_rare(&self, lists: &[String], word: &str) -> bool {
        lists.iter().filter_map(|name| self.words.get(name)).any(|list| list.is_rare(word))
    }
}

// ---------------------------------------------------------------- the user dictionary

/// How hard a negative word is pushed down.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, Default)]
#[serde(rename_all = "lowercase")]
pub enum Strength {
    /// Ranked well below what it would be; still offered.
    #[default]
    Soft,
    /// Never offered.
    Strong,
}

#[derive(Deserialize)]
struct Positive {
    word: String,
    #[serde(default)]
    language: Option<String>,
    count: Option<u32>,
    confidence: Option<String>,
}

#[derive(Deserialize)]
struct Negative {
    word: String,
    #[serde(default)]
    language: Option<String>,
    #[serde(default)]
    strength: Strength,
}

#[derive(Deserialize)]
struct DictionaryFile {
    #[serde(default)]
    positive: Vec<Positive>,
    #[serde(default)]
    negative: Vec<Negative>,
}

/// Entries past these are ignored (the dictionary contract).
const MAX_POSITIVE: usize = 20_000;
const MAX_NEGATIVE: usize = 20_000;

/// The dictionary's language names, as the layouts' `lang`.
fn lang(name: Option<&str>) -> Option<&'static str> {
    match name.unwrap_or("korean") {
        "korean" => Some("ko"),
        "english" => Some("en"),
        "japanese" => Some("ja"),
        _ => None,
    }
}

/// The user's own words (`bada-dictionary`, version 1: `docs/reference/dictionary-contract.md`).
#[derive(Debug, Default)]
pub struct UserDictionary {
    positive: HashMap<(&'static str, String), u32>,
    negative: HashMap<(&'static str, String), Strength>,
}

impl UserDictionary {
    /// Reads a dictionary object. A wrong format is refused whole.
    pub fn parse(value: &Value) -> Result<UserDictionary, Vec<Problem>> {
        if value.get("format").and_then(Value::as_str) != Some("bada-dictionary")
            || value.get("version") != Some(&Value::from(1))
        {
            return Err(vec![Problem::error("format", "bad-format", "a dictionary is bada-dictionary, version 1")]);
        }
        let file: DictionaryFile =
            serde_json::from_value(value.clone()).map_err(|e| vec![Problem::error("", "bad-value", e.to_string())])?;
        let mut d = UserDictionary::default();
        let fold = |lang: &str, word: &str| if lang == "en" { word.to_lowercase() } else { word.to_string() };
        for p in file.positive.into_iter().take(MAX_POSITIVE) {
            let (word, Some(lang)) = (p.word.trim(), lang(p.language.as_deref())) else { continue };
            if word.is_empty() {
                continue;
            }
            let count = p.count.unwrap_or(match p.confidence.as_deref() {
                Some("confirmed") => 200,
                Some("tentative") => 10,
                _ => 50,
            });
            // A word listed twice adds its counts.
            *d.positive.entry((lang, fold(lang, word))).or_insert(0) += count;
        }
        for n in file.negative.into_iter().take(MAX_NEGATIVE) {
            let (word, Some(lang)) = (n.word.trim(), lang(n.language.as_deref())) else { continue };
            if !word.is_empty() {
                d.negative.insert((lang, fold(lang, word)), n.strength);
            }
        }
        Ok(d)
    }

    pub fn summary(&self) -> DictionarySummary {
        DictionarySummary { positive: self.positive.len(), negative: self.negative.len() }
    }

    fn key(lang: &str, word: &str) -> Option<(&'static str, String)> {
        let lang = ["ko", "en", "ja"].into_iter().find(|l| lang == *l || lang.starts_with(&format!("{l}-")))?;
        Some((lang, if lang == "en" { word.to_lowercase() } else { word.to_string() }))
    }

    /// How much the user uses a word of a language (0: it isn't theirs).
    pub fn count(&self, lang: &str, word: &str) -> u32 {
        Self::key(lang, word).and_then(|k| self.positive.get(&k)).copied().unwrap_or(0)
    }

    /// Whether the user has words of a language.
    pub fn has(&self, lang: &str) -> bool {
        self.positive.keys().any(|k| lang == k.0 || lang.starts_with(&format!("{}-", k.0)))
    }

    pub fn negative(&self, lang: &str, word: &str) -> Option<Strength> {
        Self::key(lang, word).and_then(|k| self.negative.get(&k)).copied()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn a_word_list_counts_and_knows_rare_words() {
        let list = WordList::parse("# a comment\nhello\nLove\t1234\nabout\t7\n#! rare\ninterpolate\n");
        assert_eq!(
            (list.count("love"), list.count("Hello"), list.count("about"), list.count("nothing")),
            (1234, 1, 7, 0)
        );
        assert!(list.is_rare("Interpolate") && !list.is_rare("love"));
        assert_eq!(list.len(), 3);
        let korean = WordList::parse("ㅋㅋ\t9\n안녕하세요\t50\n");
        assert_eq!((korean.count("안녕하세요"), korean.count("ㅋㅋ"), korean.count("안녕")), (50, 9, 0));
    }

    #[test]
    fn the_users_words_by_language() {
        let d = UserDictionary::parse(&json!({"format": "bada-dictionary", "version": 1,
            "positive": [{"word": "데이터센터", "count": 40}, {"word": "데이터센터"}, {"word": "Kubernetes", "language": "english"},
                         {"word": "サーバー", "language": "japanese", "confidence": "confirmed"}],
            "negative": [{"word": "감소합니다", "strength": "strong"}, {"word": "god", "language": "english"}]}))
        .unwrap();
        assert_eq!(d.count("ko", "데이터센터"), 90, "a word listed twice adds its counts");
        assert_eq!((d.count("en", "kubernetes"), d.count("ja", "サーバー"), d.count("ko", "サーバー")), (50, 200, 0));
        assert_eq!(
            (d.negative("ko", "감소합니다"), d.negative("en", "God"), d.negative("ja", "god")),
            (Some(Strength::Strong), Some(Strength::Soft), None)
        );
        assert_eq!(d.summary().positive, 3);
        assert!(UserDictionary::parse(&json!({"format": "bada-dictionary", "version": 2})).is_err());
    }
}
