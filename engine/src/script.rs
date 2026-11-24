//! Scripts of Japanese text: hiragana to katakana, and the two marks of the kana layout.

/// The voiced and semi-voiced marks as characters of their own (゛ ゜).
pub const DAKUTEN: char = '\u{309B}';
pub const HANDAKUTEN: char = '\u{309C}';

/// Hiragana in the katakana block (か → カ, ゔ → ヴ); everything else stays.
pub fn katakana(hiragana: &str) -> String {
    hiragana
        .chars()
        .map(|c| if ('ぁ'..='ゖ').contains(&c) { char::from_u32(c as u32 + 0x60).unwrap_or(c) } else { c })
        .collect()
}

#[cfg(test)]
mod tests {
    #[test]
    fn scripts() {
        assert_eq!(super::katakana("らーめん"), "ラーメン");
    }
}
