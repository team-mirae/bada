//! `read: search` (the standard, §7.5.6): what was typed is a search, and the menu lists what it
//! finds. What is searched, and how its matches rank, is the layout file's `search` part; the
//! matching itself is here, the same for every search layout. From bada ime 0.3.1's layouts 150
//! and 151.

use std::collections::BTreeMap;

use crate::layout::{Search, Source};

/// A search buffer: rows of named columns. Text is kept in lower case but for `text`, the
/// character or characters a row offers.
pub struct Table {
    columns: &'static [&'static str],
    rows: Vec<Row>,
}

type Row = Vec<Box<str>>;

/// A row that matched, with what orders it: its tier, its source, its place within them.
type Match<'a> = ((u8, usize, usize), &'a Table, &'a Row, &'a Source);

/// The columns of the buffers bada knows.
const COLUMNS: [(&str, &[&str]); 3] = [
    // The symbols, hand-written: text, canonical name, tags, and the typed characters that offer it.
    ("symbols", &["text", "name", "tags", "keys"]),
    // Emoji with CLDR keywords, and their five skin-tone forms.
    ("emoji", &["text", "name", "keywords", "tones"]),
    // Every named character: code, name, and for some a colloquial name, its tags and its place.
    ("unicode-names", &["code", "name", "alias", "tags", "order"]),
];

/// Skin tones, in the order an emoji's `tones` lists them; `none` is the plain emoji.
pub const TONES: [&str; 6] = ["none", "light", "medium-light", "medium", "medium-dark", "dark"];

impl Table {
    /// A tab-separated buffer; `#` starts a comment. None for a buffer that is no search table.
    pub fn parse(name: &str, text: &str) -> Option<Table> {
        let columns = COLUMNS.iter().find(|c| c.0 == name)?.1;
        let rows = text
            .lines()
            .filter(|l| !l.starts_with('#') && !l.is_empty())
            .map(|line| {
                let mut fields: Vec<Box<str>> =
                    line.split('\t')
                        .zip(columns.iter())
                        .map(|(f, column)| {
                            if matches!(*column, "text" | "tones" | "keys") {
                                f.into()
                            } else {
                                f.to_lowercase().into()
                            }
                        })
                        .collect();
                fields.resize(columns.len(), "".into());
                fields
            })
            .collect();
        Some(Table { columns, rows })
    }

    fn column(&self, name: &str) -> Option<usize> {
        self.columns.iter().position(|c| *c == name)
    }
}

fn words(s: &str) -> impl Iterator<Item = &str> {
    s.split(|c: char| c.is_whitespace() || matches!(c, '-' | '_' | '|' | ',' | ':')).filter(|w| !w.is_empty())
}

/// Whether every query word begins one of the words.
fn begins<'a>(query: &[&str], mut text: impl FnMut() -> Box<dyn Iterator<Item = &'a str> + 'a>) -> bool {
    !query.is_empty() && query.iter().all(|q| text().any(|w| w.starts_with(q)))
}

/// How good a match is: the tiers of §7.5.6, best first.
fn tier(name: &str) -> u8 {
    match name {
        "exact" => 0,
        "whole" => 1,
        "first" => 2,
        _ => 3,
    }
}

/// A character's code as `U+XXXX`.
fn code(c: char) -> String {
    format!("U+{:04X}", c as u32)
}

/// The other forms of a letter or digit typed alone: full-width, circled; a digit's superscript,
/// subscript and roman numeral.
fn forms(c: char) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let wide = |c: char| char::from_u32(c as u32 + 0xFEE0).map(|w| (w.to_string(), "fullwidth".to_string()));
    if let Some(d) = c.to_digit(10) {
        out.extend(wide(c));
        let circled = if d == 0 { '⓪' } else { char::from_u32(0x245F + d).unwrap_or(c) };
        out.push((circled.to_string(), "circled".into()));
        out.push((["⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"][d as usize].to_string(), "superscript".into()));
        out.push((char::from_u32(0x2080 + d).unwrap_or(c).to_string(), "subscript".into()));
        if d >= 1 {
            out.push((char::from_u32(0x215F + d).unwrap_or(c).to_string(), "roman numeral".into()));
        }
    } else if c.is_ascii_lowercase() {
        out.extend(wide(c));
        out.push((char::from_u32(0x24D0 + c as u32 - 'a' as u32).unwrap_or(c).to_string(), "circled".into()));
    }
    out
}

/// What a search finds: (text, note), best first, at most the layout's `limit`.
///
/// `tones`: the skin tones offered for an emoji that has them, in order (a tone left out isn't
/// offered).
pub fn search(part: &Search, tables: &BTreeMap<String, Table>, query: &str, tones: &[String]) -> Vec<(String, String)> {
    let q = query.trim().to_lowercase();
    let mut out: Vec<(String, String)> = Vec::new();
    let push = |out: &mut Vec<(String, String)>, text: &str, note: String| {
        if out.len() < part.limit && !text.is_empty() && !out.iter().any(|o| o.0 == text) {
            out.push((text.to_string(), note));
        }
    };
    let mut chars = q.chars();
    let alone = match (chars.next(), chars.next()) {
        (Some(c), None) => Some(c),
        _ => None,
    };
    // One character typed alone: the entries that list it, and nothing else.
    if let Some(c) = alone {
        let (mut listed, mut with_forms) = (false, false);
        for source in &part.sources {
            let (Some(table), Some(single)) = (tables.get(&source.data), source.single.as_deref()) else { continue };
            listed = true;
            with_forms |= source.forms;
            let (Some(keys), Some(text), name) = (table.column(single), table.column("text"), table.column("name"))
            else {
                continue;
            };
            for row in table.rows.iter().filter(|r| r[keys].split(' ').any(|k| k.chars().eq([c]))) {
                push(&mut out, &row[text], name.map(|n| row[n].to_string()).unwrap_or_default());
            }
        }
        if listed {
            if out.is_empty() && with_forms {
                for (text, note) in forms(c) {
                    push(&mut out, &text, note);
                }
            }
            return out;
        }
    }
    // A character by its code.
    let mut by_code = false;
    if let Some(rule) = &part.code {
        let after = rule.after.iter().filter(|p| !p.is_empty()).find_map(|p| q.strip_prefix(p.as_str()));
        let digit_first = || {
            rule.digit_first_after
                .iter()
                .find_map(|p| q.strip_prefix(p.as_str()).filter(|h| h.starts_with(|c: char| c.is_ascii_digit())))
        };
        if let Some(hex) = after.or_else(digit_first) {
            if (1..=rule.max_digits).contains(&hex.len()) && hex.chars().all(|c| c.is_ascii_hexdigit()) {
                if let Some(c) = u32::from_str_radix(hex, 16).ok().and_then(char::from_u32) {
                    by_code = true;
                    push(&mut out, &c.to_string(), character_note(tables, c));
                }
            }
        }
    }
    let query: Vec<&str> = words(&q).collect();
    if query.is_empty() || (q.chars().count() < part.min_length.unwrap_or(1) && !by_code) {
        return out;
    }
    let mut found: Vec<Match> = Vec::new();
    for (at, source) in part.sources.iter().enumerate() {
        let Some(table) = tables.get(&source.data) else { continue };
        let columns: Vec<usize> = source.columns.iter().filter_map(|c| table.column(c)).collect();
        let Some(&first) = columns.first() else { continue };
        let order = table.column("order");
        for (i, row) in table.rows.iter().enumerate() {
            // A row with nothing in its first column isn't this source's (a character with no colloquial name).
            if row[first].is_empty() || !query.iter().all(|w| columns.iter().any(|&c| row[c].contains(w))) {
                continue;
            }
            if !begins(&query, || Box::new(columns.iter().flat_map(|&c| words(&row[c])))) {
                continue;
            }
            let tiers = source.tiers.iter().find(|t| match t.as_str() {
                "exact" => *row[first] == q,
                "whole" => query.len() == 1 && columns.iter().any(|&c| words(&row[c]).any(|w| w == query[0])),
                "first" => begins(&query, || Box::new(words(&row[first]))),
                _ => true,
            });
            let Some(tier_name) = tiers else { continue };
            let tie = match source.order.as_deref() {
                Some("shortest") => row[first].len(),
                _ => order.and_then(|o| row[o].parse::<usize>().ok()).unwrap_or(i),
            };
            found.push(((tier(tier_name), at, tie), table, row, source));
        }
    }
    found.sort_by_key(|f| f.0);
    for (_, table, row, source) in found {
        if out.len() >= part.limit {
            break;
        }
        // A character named by its code, or a row with its own text.
        let character =
            table.column("code").and_then(|c| u32::from_str_radix(&row[c], 16).ok()).and_then(char::from_u32);
        if let Some(c) = character {
            push(&mut out, &c.to_string(), row_note(table, row, c));
            continue;
        }
        let (Some(text), name) = (table.column("text"), table.column("name")) else { continue };
        let name = name.map(|n| row[n].to_string()).unwrap_or_default();
        let forms: Vec<&str> =
            table.column("tones").map(|t| row[t].split(' ').filter(|f| !f.is_empty()).collect()).unwrap_or_default();
        if source.tones.is_some() && forms.len() == 5 {
            for tone in tones {
                match TONES.iter().position(|t| t == tone) {
                    Some(0) => push(&mut out, &row[text], name.clone()),
                    Some(i) => push(&mut out, forms[i - 1], format!("{name}: {} skin tone", TONES[i])),
                    None => {}
                }
            }
        } else {
            push(&mut out, &row[text], name);
        }
    }
    out
}

/// `U+XXXX name`, with the colloquial name first where the character has one.
fn row_note(table: &Table, row: &[Box<str>], c: char) -> String {
    let field = |name: &str| table.column(name).map_or("", |i| &*row[i]);
    let (name, alias) = (field("name"), field("alias"));
    let named = if alias.is_empty() { name.to_string() } else { format!("{alias} ({name})") };
    format!("{} {named}", code(c)).trim_end().to_string()
}

/// The note of a character found by its code: its row among the named characters, if it has one.
fn character_note(tables: &BTreeMap<String, Table>, c: char) -> String {
    let row = tables.get("unicode-names").and_then(|table| {
        let hex = format!("{:04x}", c as u32);
        let at = table.column("code")?;
        Some((table, table.rows.iter().find(|r| r[at].eq_ignore_ascii_case(&hex))?))
    });
    row.map_or_else(|| code(c), |(table, row)| row_note(table, row, c))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::stock;

    fn tables() -> BTreeMap<String, Table> {
        let symbols = "$\tusd\tdollar money cash currency bill\t¥ $\n€\teur\teuro money\t¥\n。\tideographic full stop\tperiod kuten\t.\n…\tellipsis\tdots\t.\n";
        let emoji = "😀\tgrinning face\tface|grin|happy|smile\t\n👋\twaving hand\thand|wave\t👋🏻 👋🏼 👋🏽 👋🏾 👋🏿\n";
        let unicode = "0041\tLATIN CAPITAL LETTER A\n1F600\tGRINNING FACE\n00B7\tMIDDLE DOT\n321C\tPARENTHESIZED HANGUL CIEUC U\tjusikhoesa\tju joo jusik corporation\n3231\tPARENTHESIZED IDEOGRAPH STOCK\tkabushikigaisha\tkabu corporation\nD3C9\tHANGUL SYLLABLE PYEONG\tpyeong\tpyung tsubo area\n576A\tCJK UNIFIED IDEOGRAPH-576A\ttsubo\tpyeong pyung area\n";
        [("symbols", symbols), ("emoji", emoji), ("unicode-names", unicode)]
            .into_iter()
            .map(|(name, text)| (name.to_string(), Table::parse(name, text).unwrap()))
            .collect()
    }

    fn find(layout: &str, query: &str, tones: &[&str]) -> Vec<(String, String)> {
        let layout = stock().iter().find(|l| l.id() == layout).unwrap();
        let tones: Vec<String> = tones.iter().map(|t| t.to_string()).collect();
        search(layout.file.search.as_ref().unwrap(), &tables(), query, &tones)
    }

    fn texts(layout: &str, query: &str) -> Vec<String> {
        find(layout, query, &TONES).into_iter().map(|f| f.0).collect()
    }

    // bada ime 0.3.1's own cases (engine/src/search.rs), on the three layout files.

    #[test]
    fn symbols_by_key_name_and_tag() {
        assert_eq!(texts("uni-symbols", "."), ["。", "…"]);
        assert_eq!(texts("uni-symbols", "¥"), ["$", "€"]);
        assert_eq!(texts("uni-symbols", "usd"), ["$"]);
        assert_eq!(texts("uni-symbols", "money"), ["$", "€"]);
        assert_eq!(find("uni-symbols", "usd", &[])[0].1, "usd");
        assert!(texts("uni-symbols", "1").contains(&"①".to_string()));
        assert_eq!(texts("uni-symbols", "e"), ["ｅ", "ⓔ"], "a letter alone: its forms");
        assert!(texts("uni-symbols", "happy face").is_empty(), "emoji are another layout's");
    }

    #[test]
    fn emoji_and_their_skin_tones() {
        assert_eq!(texts("uni-emojis", "happy face"), ["😀"]);
        let got: Vec<String> = find("uni-emojis", "wave", &["medium", "none"]).into_iter().map(|f| f.0).collect();
        assert_eq!(got, ["👋🏽", "👋"]);
        assert_eq!(find("uni-emojis", "wave", &["medium"])[0].1, "waving hand: medium skin tone");
        assert!(texts("uni-emojis", "w").is_empty(), "a search of one character looks no names up");
    }

    #[test]
    fn unicode_by_code_and_name() {
        assert_eq!(find("uni-unicode", "1f600", &[])[0], ("😀".to_string(), "U+1F600 grinning face".to_string()));
        assert_eq!(texts("uni-unicode", "u+41")[0], "A");
        assert_eq!(texts("uni-unicode", "middle dot")[0], "·");
        assert_eq!(texts("uni-unicode", "face")[0], "😀", "`face` is hex too, but starts with a letter: a name");
        // Colloquial names come before Unicode's own.
        assert_eq!(texts("uni-unicode", "ju")[0], "㈜");
        assert_eq!(texts("uni-unicode", "joo"), ["㈜"]);
        assert_eq!(texts("uni-unicode", "corporation"), ["㈜", "㈱"]);
        assert_eq!(texts("uni-unicode", "pyeong"), ["평", "坪"]);
        assert_eq!(texts("uni-unicode", "pyung"), ["평", "坪"]);
        assert_eq!(find("uni-unicode", "ju", &[])[0].1, "U+321C jusikhoesa (parenthesized hangul cieuc u)");
        assert!(texts("uni-unicode", "a").is_empty(), "a name search needs two characters");
    }
}
