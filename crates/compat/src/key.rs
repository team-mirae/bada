//! Key events (the standard, §4). Keys are physical positions, named by their W3C
//! `KeyboardEvent.code`, on a JIS keyboard.

use serde::{Deserialize, Serialize};

macro_rules! keys {
    ($($name:ident = $code:literal),* $(,)?) => {
        /// A physical key the standard names. Any other code is [`Key::Other`].
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
        pub enum Key { $($name,)* Other }

        impl Key {
            /// Every named key, in order (the 48 typing keys first).
            pub const ALL: &'static [Key] = &[$(Key::$name),*];

            /// The canonical `KeyboardEvent.code` (`"Other"` for [`Key::Other`]).
            pub fn code(self) -> &'static str {
                match self { $(Key::$name => $code,)* Key::Other => "Other" }
            }

            /// Any string; codes the standard doesn't name are [`Key::Other`].
            pub fn from_code(code: &str) -> Key {
                match code { $($code => Key::$name,)* _ => Key::Other }
            }
        }
    };
}

keys! {
    // The 48 typing keys: the keys a layout types on.
    Digit1 = "Digit1", Digit2 = "Digit2", Digit3 = "Digit3", Digit4 = "Digit4", Digit5 = "Digit5",
    Digit6 = "Digit6", Digit7 = "Digit7", Digit8 = "Digit8", Digit9 = "Digit9", Digit0 = "Digit0",
    Minus = "Minus", Equal = "Equal", IntlYen = "IntlYen",
    KeyQ = "KeyQ", KeyW = "KeyW", KeyE = "KeyE", KeyR = "KeyR", KeyT = "KeyT",
    KeyY = "KeyY", KeyU = "KeyU", KeyI = "KeyI", KeyO = "KeyO", KeyP = "KeyP",
    BracketLeft = "BracketLeft", BracketRight = "BracketRight",
    KeyA = "KeyA", KeyS = "KeyS", KeyD = "KeyD", KeyF = "KeyF", KeyG = "KeyG",
    KeyH = "KeyH", KeyJ = "KeyJ", KeyK = "KeyK", KeyL = "KeyL",
    Semicolon = "Semicolon", Quote = "Quote", Backslash = "Backslash",
    KeyZ = "KeyZ", KeyX = "KeyX", KeyC = "KeyC", KeyV = "KeyV",
    KeyB = "KeyB", KeyN = "KeyN", KeyM = "KeyM",
    Comma = "Comma", Period = "Period", Slash = "Slash", IntlRo = "IntlRo",
    Space = "Space", Enter = "Enter", Backspace = "Backspace", Tab = "Tab", Escape = "Escape",
    ArrowUp = "ArrowUp", ArrowDown = "ArrowDown", ArrowLeft = "ArrowLeft", ArrowRight = "ArrowRight",
    Delete = "Delete", Home = "Home", End = "End",
    F1 = "F1", F2 = "F2", F3 = "F3", F4 = "F4", F5 = "F5", F6 = "F6",
    F7 = "F7", F8 = "F8", F9 = "F9", F10 = "F10", F11 = "F11", F12 = "F12",
    // The language keys and the lock. A harness reads them (§9) and never sends them: they are
    // named here so that an engine can tell one from a key it has simply never heard of.
    Backquote = "Backquote", NonConvert = "NonConvert", Convert = "Convert", KanaMode = "KanaMode",
    Lang2 = "Lang2", Lang1 = "Lang1", CapsLock = "CapsLock",
}

impl Key {
    /// One of the 48 typing keys.
    pub fn is_typing(self) -> bool {
        (self as usize) <= (Key::IntlRo as usize)
    }

    /// `KeyA`…`KeyZ`, as a lowercase letter.
    pub fn letter(self) -> Option<char> {
        let l = self.code().strip_prefix("Key").filter(|l| l.len() == 1)?;
        Some(l.chars().next()?.to_ascii_lowercase())
    }

    /// A language key or Caps Lock: the harness's, never the engine's.
    pub fn is_harness_key(self) -> bool {
        (self as usize) >= (Key::Backquote as usize) && self != Key::Other
    }
}

/// Modifier state for one key event. Missing fields are false.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(default)]
pub struct Mods {
    pub shift: bool,
    pub ctrl: bool,
    pub alt: bool,
    pub meta: bool,
}

impl Mods {
    pub const NONE: Mods = Mods { shift: false, ctrl: false, alt: false, meta: false };
    pub const SHIFT: Mods = Mods { shift: true, ctrl: false, alt: false, meta: false };

    /// ⌃ or ⌘: a shortcut for the app, passed on as the physical key (§7.2).
    pub fn is_shortcut(self) -> bool {
        self.ctrl || self.meta
    }

    /// No modifier but ⇧.
    pub fn is_plain(self) -> bool {
        !(self.ctrl || self.alt || self.meta)
    }
}

/// `{"type": "key", "key": "KeyA", "shift": false, "ctrl": false, "alt": false, "meta": false}`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct KeyEvent {
    /// The `KeyboardEvent.code` as sent: any string.
    #[serde(rename = "key")]
    pub code: String,
    #[serde(flatten)]
    pub mods: Mods,
    /// A repeat of a held key.
    #[serde(default)]
    pub repeat: bool,
}

impl KeyEvent {
    pub fn new(code: &str, mods: Mods) -> KeyEvent {
        KeyEvent { code: code.to_string(), mods, repeat: false }
    }

    pub fn key(&self) -> Key {
        Key::from_code(&self.code)
    }
}
