//! Replies (the standard, §6), and what an engine says about itself (§3.2).

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::Mode;

/// Text by language (BCP 47 tags); `en` is always there.
pub type Texts = BTreeMap<String, String>;

/// `{"en": …}`.
pub fn en(text: &str) -> Texts {
    BTreeMap::from([("en".to_string(), text.to_string())])
}

/// The error codes of §6.2.
pub mod code {
    pub const BAD_JSON: &str = "bad-json";
    pub const BAD_EVENT: &str = "bad-event";
    pub const UNKNOWN_EVENT: &str = "unknown-event";
    pub const NEEDS_ENGINE: &str = "needs-engine";
    pub const UNKNOWN_ACTION: &str = "unknown-action";
    pub const UNKNOWN_PROFILE: &str = "unknown-profile";
    pub const UNKNOWN_LAYOUT: &str = "unknown-layout";
    pub const NO_PROFILE: &str = "no-profile";
    pub const BAD_ROW: &str = "bad-row";
    pub const INVALID: &str = "invalid";
    pub const PANIC: &str = "panic";
}

/// Why nothing changed (§6.2).
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Error {
    pub code: String,
    pub message: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub path: Option<String>,
}

impl Error {
    pub fn new(code: &str, message: impl Into<String>) -> Error {
        Error { code: code.to_string(), message: message.into(), path: None }
    }

    pub fn at(code: &str, path: &str, message: impl Into<String>) -> Error {
        Error { code: code.to_string(), message: message.into(), path: Some(path.to_string()) }
    }
}

/// What `commit` replaces.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Replace {
    /// The marked text, or the selection when nothing is marked.
    #[default]
    Marked,
    /// The selection, even when text is marked.
    Selection,
}

/// The writing direction of a reply's text (§6.5). Any value but `"rtl"` reads as left to right.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase", from = "String")]
pub enum Dir {
    #[default]
    Ltr,
    Rtl,
}

impl From<String> for Dir {
    fn from(value: String) -> Self {
        if value == "rtl" { Dir::Rtl } else { Dir::Ltr }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SegmentKind {
    /// Still being read.
    Live,
    /// It no longer changes as more keys are typed.
    Fixed,
    /// The row the user moved to.
    Picked,
}

/// A part of the preedit: `len` in Unicode scalar values.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Segment {
    pub len: usize,
    pub kind: SegmentKind,
}

/// The marked text after the commit.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Preedit {
    /// `""`: nothing is marked.
    #[serde(default)]
    pub text: String,
    /// The caret's place in `text`, in Unicode scalar values. A reader takes a missing one as the
    /// end; a writer always writes it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub caret: Option<usize>,
    /// `text` cut into parts, in order; their lengths add up to its length.
    #[serde(default)]
    pub segments: Vec<Segment>,
}

impl Default for Preedit {
    /// Nothing marked.
    fn default() -> Self {
        Preedit { text: String::new(), caret: Some(0), segments: Vec::new() }
    }
}

impl Preedit {
    /// One live segment, with the caret at the end.
    pub fn live(text: &str) -> Preedit {
        Preedit::of(text, SegmentKind::Live)
    }

    /// One segment of `kind`, with the caret at the end.
    pub fn of(text: &str, kind: SegmentKind) -> Preedit {
        let len = text.chars().count();
        let segments = if len == 0 { vec![] } else { vec![Segment { len, kind }] };
        Preedit { text: text.to_string(), caret: Some(len), segments }
    }

    /// The caret's place, wherever it was written or not.
    pub fn caret(&self) -> usize {
        self.caret.unwrap_or_else(|| self.text.chars().count())
    }
}

/// One row of the menu.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Row {
    pub text: String,
    /// The signed index: 0 the top reading, 1 and up the others, −1 and down a layout's forms.
    pub entry: i32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub note: Option<String>,
    /// BCP 47: the harness picks the font.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lang: Option<String>,
    /// The layout whose reading this is.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layout: Option<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct Menu {
    pub rows: Vec<Row>,
    /// The highlighted row, or none.
    pub selected: Option<usize>,
    /// The menu is one row of single characters.
    pub strip: bool,
}

/// The engine needs text from the harness to finish; answered with one `text` event.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Ask {
    /// The selected text, and what for.
    Selection(String),
    /// The text around the caret: how many characters.
    Around { before: u32, after: u32 },
}

/// The reply to an event about a text field (§6.1). An engine fills the fields it has; the layer
/// writes `ok`, `mode` and `profile`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct Reply {
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<Error>,
    /// Text to insert now.
    pub commit: String,
    pub replace: Replace,
    /// The engine took the key. false: the app gets the key too, after the commit.
    pub consumed: bool,
    /// With `consumed: false` on a typing key: the text the key should type. None: no say.
    pub types: Option<String>,
    pub preedit: Preedit,
    pub menu: Menu,
    /// As the harness last selected them: the layer echoes them.
    pub mode: Option<Mode>,
    pub profile: Option<String>,
    /// What `flush` would commit now, character for character.
    pub staged: String,
    pub ask: Option<Ask>,
    pub dir: Dir,
}

impl Default for Reply {
    fn default() -> Self {
        Reply {
            ok: true,
            error: None,
            commit: String::new(),
            replace: Replace::Marked,
            consumed: false,
            types: None,
            preedit: Preedit::default(),
            menu: Menu::default(),
            mode: None,
            profile: None,
            staged: String::new(),
            ask: None,
            dir: Dir::Ltr,
        }
    }
}

impl Reply {
    /// The key was taken, and nothing shows.
    pub fn consumed() -> Reply {
        Reply { consumed: true, ..Reply::default() }
    }

    /// The key goes to the app, after `commit` is inserted.
    pub fn pass(commit: String) -> Reply {
        Reply { commit, ..Reply::default() }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Level {
    /// Nothing was changed.
    Error,
    /// It was taken, with this noted.
    Warning,
}

/// What was wrong with a settings object, a layout or a dictionary (§6.6).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Problem {
    pub path: String,
    pub code: String,
    pub message: String,
    pub level: Level,
}

impl Problem {
    pub fn error(path: &str, code: &str, message: impl Into<String>) -> Problem {
        Problem { path: path.to_string(), code: code.to_string(), message: message.into(), level: Level::Error }
    }

    pub fn warning(path: &str, code: &str, message: impl Into<String>) -> Problem {
        Problem { path: path.to_string(), code: code.to_string(), message: message.into(), level: Level::Warning }
    }
}

/// One named action of a layout.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ActionInfo {
    pub id: String,
    pub label: Texts,
}

/// A layout, as `P_describe` and the replies to `layout` events list it.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct LayoutInfo {
    pub id: String,
    pub name: Texts,
    /// Muhenkan: it types directly. Henkan: it composes.
    pub kind: Mode,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub read: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lang: Option<String>,
    #[serde(default)]
    pub actions: Vec<ActionInfo>,
    /// The layout's options, as its file declares them.
    #[serde(default)]
    pub options: Value,
    /// One of the layouts shipped with the engine.
    #[serde(default)]
    pub stock: bool,
    /// A stock layout that a custom layout of the same id replaces.
    #[serde(default)]
    pub modified: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct DictionarySummary {
    pub positive: usize,
    pub negative: usize,
}

/// The reply to a session event (§6.6): `settings`, `layout` or `dictionary`.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct SessionReply {
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<Error>,
    /// The `engine` part in effect.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub settings: Option<Value>,
    /// The session's layouts, stock and custom.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub layouts: Option<Vec<LayoutInfo>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub dictionary: Option<DictionarySummary>,
    pub problems: Vec<Problem>,
}

impl Default for SessionReply {
    fn default() -> Self {
        SessionReply { ok: true, error: None, settings: None, layouts: None, dictionary: None, problems: vec![] }
    }
}

impl SessionReply {
    /// Marks the reply `invalid` if any problem is error-level: nothing was changed.
    pub fn with_problems(mut self, problems: Vec<Problem>) -> SessionReply {
        let errors = problems.iter().filter(|p| p.level == Level::Error).count();
        if errors > 0 {
            self.ok = false;
            self.error =
                Some(Error::new(code::INVALID, format!("{errors} problem{}", if errors == 1 { "" } else { "s" })));
        }
        self.problems = problems;
        self
    }
}

// ---------------------------------------------------------------- P_describe

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EngineInfo {
    /// Reverse-DNS: `com.teammirae.bada`.
    pub id: String,
    /// The engine's own version, not the standard's.
    pub version: String,
    pub name: Texts,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct DataInfo {
    pub name: String,
    /// false: without it, its feature is off.
    pub required: bool,
    /// The engine can use the buffer in place when it is passed with `P_BORROWED`.
    #[serde(default)]
    pub borrow: bool,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct SettingsInfo {
    /// `bada-settings/2`.
    pub format: String,
    /// A JSON Schema for the `engine` part of the settings file.
    pub schema: Value,
    /// The `engine` part of a new install.
    pub defaults: Value,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Limits {
    /// How many henkan profiles the engine takes: at least 32.
    pub profiles: u32,
}

impl Default for Limits {
    fn default() -> Self {
        Limits { profiles: 32 }
    }
}

/// What an engine says about itself (§3.2). The layer adds `abi`.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Describe {
    pub engine: EngineInfo,
    /// The event types it takes. `key`, `select`, `flush`, `reset` and `state` are required.
    pub events: Vec<String>,
    /// `segments`, `around`, `custom-layouts`, `rtl`.
    pub features: Vec<String>,
    pub keyboards: Vec<String>,
    pub data: Vec<DataInfo>,
    pub layouts: Vec<LayoutInfo>,
    /// The engine's own actions, apart from its layouts'.
    pub actions: Vec<String>,
    pub settings: SettingsInfo,
    #[serde(default)]
    pub limits: Limits,
}
