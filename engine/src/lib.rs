//! bada's engine: keys in, text out (`docs/bada-compatible-v2.md`). Every layout is read from its
//! `bada-layout/2` file by a few general ways of reading keys; no layout has code of its own.
//!
//! It is exported with `bada-compat`, like any engine: the C functions are `bada_describe`,
//! `bada_data_new`, `bada_session_new`, `bada_new`, `bada_send` and their `_free`s.
//!
//! **What this build does not do yet**: it reads the word lists and the user dictionary, but no
//! other data. So no kanji is converted, a search finds nothing, and layouts in one profile are
//! ranked by their `rank` scores alone, with no key models. What the keys type, in every layout,
//! is all here.

pub mod data;
pub mod field;
pub mod hangul;
pub mod kanji;
pub mod keymodel;
pub mod layout;
pub mod read;
pub mod romaji;
pub mod script;
pub mod session;
pub mod width;

use std::collections::BTreeMap;
use std::sync::Arc;

use bada_compat::{
    Buffer, Cause, Change, DataInfo, Describe, EngineInfo, Error, Harness, KeyEvent, Limits, Mode, Reply, SessionReply,
    SettingsInfo, Text,
};
use serde_json::{Value, json};

pub use data::{BUFFERS, Data};
pub use field::Field;
pub use session::{PROFILES, Session, Settings};

/// One text field of bada.
pub struct Bada(Field);

impl bada_compat::Engine for Bada {
    type Data = Data;
    type Session = Session;

    fn describe() -> Describe {
        let schema: Value =
            serde_json::from_str(include_str!("../../docs/schemas/settings.schema.json")).unwrap_or_default();
        Describe {
            engine: EngineInfo {
                id: "com.teammirae.bada".into(),
                version: env!("CARGO_PKG_VERSION").into(),
                name: BTreeMap::from([
                    ("en".into(), "bada".into()),
                    ("ko".into(), "바다".into()),
                    ("ja".into(), "バダ".into()),
                ]),
            },
            events: bada_compat::TYPES.map(String::from).into(),
            features: vec!["segments".into(), "custom-layouts".into()],
            keyboards: vec!["jis".into()],
            data: BUFFERS
                .iter()
                .map(|&(name, borrow)| DataInfo { name: name.into(), required: false, borrow })
                .collect(),
            layouts: layout::stock().iter().map(|l| l.info()).collect(),
            actions: ["menu.next", "menu.prev", "fix", "width.full", "width.half"].map(String::from).into(),
            settings: SettingsInfo {
                format: bada_compat::SETTINGS_FORMAT.into(),
                schema: schema["properties"]["engine"].clone(),
                defaults: json!({"muhenkan": {"layout": null}, "henkan": {"defaults": {}, "profiles": []}}),
            },
            limits: Limits { profiles: PROFILES as u32 },
        }
    }

    fn data(buffers: &[Buffer<'_>]) -> Option<Data> {
        Some(Data::new(buffers))
    }

    fn session(data: Option<Arc<Data>>, harness: &Harness) -> Session {
        Session::new(harness, data)
    }

    fn settings(session: &Session, change: Change, dry: bool) -> SessionReply {
        session.settings(change, dry)
    }

    fn layout(session: &Session, change: Change, dry: bool) -> SessionReply {
        session.layout(change, dry)
    }

    fn dictionary(session: &Session, change: Change) -> SessionReply {
        session.dictionary(change)
    }

    fn new(session: Arc<Session>) -> Self {
        Bada(Field::new(session))
    }

    fn key(&mut self, event: &KeyEvent) -> Result<Reply, Error> {
        self.0.key(event)
    }

    fn select(&mut self, mode: Mode, profile: Option<&str>) -> Result<Reply, Error> {
        self.0.select(mode, profile)
    }

    fn flush(&mut self, cause: Cause) -> Reply {
        self.0.flush(cause)
    }

    fn reset(&mut self) -> Reply {
        self.0.reset()
    }

    fn action(&mut self, name: &str) -> Result<Reply, Error> {
        self.0.action(name)
    }

    fn pick(&mut self, row: usize) -> Result<Reply, Error> {
        self.0.pick(row)
    }

    fn text(&mut self, text: &Text) -> Reply {
        self.0.text(text)
    }
}

bada_compat::export! {
    prefix: "bada",
    engine: Bada,
}
