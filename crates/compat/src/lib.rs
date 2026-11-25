//! The contract layer of **bada-compatible v2** (`docs/bada-compatible-v2.md`): its types, the C ABI,
//! the session and the panic guard.
//!
//! An engine implements [`Engine`] and exports it with [`export!`]; the layer does everything the
//! standard asks of every engine, so no engine writes it twice:
//!
//! - it reads each event's JSON and answers `bad-json`, `bad-event`, `unknown-event` and
//!   `needs-engine` itself (§6.2);
//! - it keeps each reply alive until the next `P_send` (§3.4);
//! - it echoes `mode` and `profile` as last selected, and answers `state` from the last reply;
//! - it catches panics: the reply is `panic`, and the engine is made anew with its last `select`.
//!
//! bada's own engine uses this crate like any third-party engine would. A change here is a change
//! to the standard, and goes to `docs/` first.

mod event;
pub mod ffi;
mod key;
mod reply;

use std::sync::Arc;

use serde::{Deserialize, Serialize};

pub use event::{Cause, Change, Event, FieldEvent, REQUIRED, SessionEvent, TYPES, Text, merge_patch, parse};
pub use key::{Key, KeyEvent, Mods};
pub use reply::{
    ActionInfo, Ask, DataInfo, Describe, DictionarySummary, Dir, EngineInfo, Error, LayoutInfo, Level, Limits, Menu,
    Preedit, Problem, Replace, Reply, Row, Segment, SegmentKind, SessionReply, SettingsInfo, Texts, code, en,
};

/// The standard's name and version, as `P_describe` reports them (§1).
pub const ABI_NAME: &str = "bada-compatible";
pub const MAJOR: u32 = 2;
pub const MINOR: u32 = 0;

/// The file formats' names.
pub const SETTINGS_FORMAT: &str = "bada-settings/2";
pub const LAYOUT_FORMAT: &str = "bada-layout/3";
pub const STATE_FORMAT: &str = "bada-compatible-state/2";

/// The two modes (§2).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Mode {
    /// No composition: keys type directly, or pass to the app.
    Muhenkan,
    /// Keys compose into a preedit, which is committed.
    Henkan,
}

/// What the harness says about itself in `P_session_new` (§3.3).
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Harness {
    pub id: String,
    pub version: String,
    /// `segments`, `pick`, `around`, `rtl`. An engine doesn't ask a harness for what it lacks.
    #[serde(default)]
    pub features: Vec<String>,
}

impl Harness {
    pub fn has(&self, feature: &str) -> bool {
        self.features.iter().any(|f| f == feature)
    }
}

/// One named buffer of `P_data_new` (§11).
#[derive(Clone, Copy, Debug)]
pub struct Buffer<'a> {
    pub name: &'a str,
    pub bytes: &'a [u8],
    /// Passed with `P_BORROWED`: it outlives the data.
    pub borrowed: bool,
}

impl Buffer<'_> {
    /// The bytes for as long as the data lives, when the harness lent them: an engine may keep
    /// pointers into them instead of copying.
    pub fn lent(&self) -> Option<&'static [u8]> {
        // SAFETY: with P_BORROWED the harness promises the buffer outlives the Data (§3.4), and
        // the engine keeps the slice no longer than its Data.
        self.borrowed.then(|| unsafe { std::slice::from_raw_parts(self.bytes.as_ptr(), self.bytes.len()) })
    }
}

/// An engine. Implement this, then export it with [`export!`].
///
/// The three objects of §3.1 are `Data` (immutable, shared), `Session` (what every text field of a
/// process shares: settings, custom layouts, the user dictionary) and the engine itself, one text
/// field. A session locks itself: its events take `&Session` from any thread.
pub trait Engine: Sized + Send + 'static {
    type Data: Send + Sync + 'static;
    type Session: Send + Sync + 'static;

    /// `P_describe`: what the engine is and has. Called once; the layer adds `abi`.
    fn describe() -> Describe;

    /// `P_data_new`: builds the data from the named buffers. A name the engine doesn't know is
    /// ignored, and a missing optional buffer turns its feature off. `None` fails (the harness gets
    /// NULL): a required buffer is missing or can't be read.
    fn data(buffers: &[Buffer<'_>]) -> Option<Self::Data>;

    /// `P_session_new`: a session with the engine's defaults. The layer then feeds it the
    /// harness's layouts, settings and dictionary through the three methods below.
    fn session(data: Option<Arc<Self::Data>>, harness: &Harness) -> Self::Session;

    /// A `settings` event. Only called when `describe().events` lists it; likewise the next two.
    fn settings(session: &Self::Session, change: Change, dry: bool) -> SessionReply {
        let _ = (session, change, dry);
        SessionReply::default()
    }

    /// A `layout` event.
    fn layout(session: &Self::Session, change: Change, dry: bool) -> SessionReply {
        let _ = (session, change, dry);
        SessionReply::default()
    }

    /// A `dictionary` event.
    fn dictionary(session: &Self::Session, change: Change) -> SessionReply {
        let _ = (session, change);
        SessionReply::default()
    }

    /// `P_new`: one text field, in muhenkan with no profile until its first `select`.
    fn new(session: Arc<Self::Session>) -> Self;

    /// One key. An unknown code is a key the engine doesn't handle: it commits what's staged if
    /// that's its rule, and passes the key on (`consumed: false`). `no-profile` is its one error:
    /// the engine's profile was removed, and it has had no `select` since.
    fn key(&mut self, event: &KeyEvent) -> Result<Reply, Error>;

    /// The mode and profile of this field. Commits what's staged if either changes; nothing
    /// happens if neither does. `unknown-profile` and `no-profile` are its errors.
    fn select(&mut self, mode: Mode, profile: Option<&str>) -> Result<Reply, Error>;

    /// Commits what is staged; afterwards nothing is staged.
    fn flush(&mut self, cause: Cause) -> Reply;

    /// Drops what is staged; commits nothing.
    fn reset(&mut self) -> Reply;

    /// A request by name. A name the engine doesn't have is `unknown-action`.
    fn action(&mut self, name: &str) -> Result<Reply, Error> {
        Err(Error::new(code::UNKNOWN_ACTION, format!("no action named {name}")))
    }

    /// Moves to a row of the menu. A row it doesn't have is `bad-row`.
    fn pick(&mut self, row: usize) -> Result<Reply, Error> {
        Err(Error::new(code::BAD_ROW, format!("the menu has no row {row}")))
    }

    /// The harness's answer to a reply's `ask`.
    fn text(&mut self, text: &Text) -> Reply {
        let _ = text;
        Reply::consumed()
    }
}

/// The C header for an engine exported under `prefix`: what `export!` generates, for a harness
/// written in C, C++ or Swift.
pub fn header(prefix: &str) -> String {
    let guard = format!("{}_H", prefix.to_uppercase());
    format!(
        r#"/* {prefix}: a bada-compatible v{MAJOR}.{MINOR} engine (docs/bada-compatible-v2.md).
 * Written by bada-compat's export!: don't edit it. */
#ifndef {guard}
#define {guard}

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {{
#endif

typedef struct {prefix}_data {prefix}_data;
typedef struct {prefix}_session {prefix}_session;
typedef struct {prefix}_engine {prefix}_engine;

/* One named buffer of {prefix}_data_new. */
typedef struct {{ const char *name; const uint8_t *ptr; size_t len; uint32_t flags; }} {prefix}_buf;
/* The buffer outlives the data: the engine may keep pointers into it. */
#define {upper}_BORROWED 1u

/* Static JSON: what the engine is and has. No engine needed. */
const char *{prefix}_describe(void);

/* Immutable, shared, thread-safe. NULL on failure. */
const {prefix}_data *{prefix}_data_new(const {prefix}_buf *bufs, size_t n);
void {prefix}_data_free(const {prefix}_data *data);

/* Settings, layouts and the user dictionary of one process. NULL if init_json has no harness. */
{prefix}_session *{prefix}_session_new(const {prefix}_data *data, const char *init_json);
void {prefix}_session_free({prefix}_session *session);

/* One text field. */
{prefix}_engine *{prefix}_new({prefix}_session *session);
void {prefix}_free({prefix}_engine *engine);

/* One event in, one reply out. The reply is valid until the next call on the same engine, or on
 * the session when engine is NULL. */
const char *{prefix}_send({prefix}_session *session, {prefix}_engine *engine_or_null, const char *event_json);

#ifdef __cplusplus
}}
#endif

#endif
"#,
        upper = prefix.to_uppercase(),
    )
}
