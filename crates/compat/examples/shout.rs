//! shout: a small bada-compatible v2 engine, written on `bada-compat` the way any engine is. It is
//! the layer's own test engine and the conformance runner's first one; it is also the shortest
//! example of the three objects (no data, a session with settings, an engine per text field).
//!
//! Henkan composes letters as CAPITALS in the preedit, with a menu of the letters' forms. Tab and
//! ↓ move down the menu, ⇧Tab and ↑ up, without wrapping; Backspace takes a letter back; ⇧Enter
//! commits and does nothing else; any other key commits what shows, then acts. In muhenkan nothing
//! is consumed. The action `shout` shouts the selection, which it asks the harness for.
//!
//! `cargo build -p bada-compat --example shout` builds it as a library, prefix `shout`.

use std::sync::{Arc, RwLock};

use bada_compat::{
    Ask, Cause, Change, Describe, EngineInfo, Error, Harness, Key, KeyEvent, Limits, Menu, Mode, Preedit, Problem,
    Replace, Reply, Row, SegmentKind, SessionReply, SettingsInfo, Text, code, merge_patch,
};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

/// One way to write the letters: one menu row.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Form {
    Upper,
    Lower,
    Title,
}

impl Form {
    fn write(self, letters: &str) -> String {
        match self {
            Form::Upper => letters.to_uppercase(),
            Form::Lower => letters.to_lowercase(),
            Form::Title => {
                let mut c = letters.chars();
                c.next().map(|f| f.to_uppercase().chain(c.flat_map(char::to_lowercase)).collect()).unwrap_or_default()
            }
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub id: String,
    pub name: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Henkan {
    pub profiles: Vec<Profile>,
}

/// The `engine` part of the settings file, as shout has it: the menu's rows, top first, and the
/// henkan profiles, of which a harness reads `id` and `name` (§8).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Settings {
    pub forms: Vec<Form>,
    pub henkan: Henkan,
}

impl Default for Settings {
    fn default() -> Self {
        Settings { forms: vec![Form::Upper, Form::Lower], henkan: Henkan::default() }
    }
}

const PROFILES: usize = 32;

/// What is wrong with settings, if anything.
fn check(value: &Value) -> Result<Settings, Vec<Problem>> {
    let settings: Settings = serde_json::from_value(value.clone()).map_err(|e| {
        let message = e.to_string();
        let code = if message.starts_with("unknown field") { "unknown-field" } else { "bad-value" };
        vec![Problem::error("", code, message)]
    })?;
    let mut problems = Vec::new();
    if settings.forms.is_empty() {
        problems.push(Problem::error("forms", "bad-value", "the menu needs a row"));
    }
    if settings.forms.iter().enumerate().any(|(i, f)| settings.forms[..i].contains(f)) {
        problems.push(Problem::error("forms", "bad-value", "a form is listed twice"));
    }
    let profiles = &settings.henkan.profiles;
    if profiles.len() > PROFILES {
        problems.push(Problem::error(
            "henkan.profiles",
            "too-many-profiles",
            format!("shout takes {PROFILES} profiles"),
        ));
    }
    for (i, p) in profiles.iter().enumerate() {
        let path = format!("henkan.profiles[{i}].id");
        let shaped = (1..=32).contains(&p.id.len())
            && p.id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-');
        if !shaped {
            problems.push(Problem::error(&path, "bad-value", "an id is 1 to 32 of a-z, 0-9 and -"));
        }
        if profiles[..i].iter().any(|q| q.id == p.id) {
            problems.push(Problem::error(&path, "duplicate-profile", format!("two profiles are named {}", p.id)));
        }
    }
    if problems.is_empty() { Ok(settings) } else { Err(problems) }
}

/// What every text field shares: the settings, and what was wrong with the last change.
pub struct Session {
    settings: RwLock<Settings>,
    problems: RwLock<Vec<Problem>>,
}

pub struct Shout {
    session: Arc<Session>,
    mode: Mode,
    profile: Option<String>,
    /// The staged letters, lowercase.
    letters: String,
    /// The menu row showing.
    row: usize,
    /// Waiting for the harness's selection (the `shout` action).
    waiting: bool,
}

impl Shout {
    fn forms(&self) -> Vec<Form> {
        self.session.settings.read().unwrap().forms.clone()
    }

    fn shown(&self) -> String {
        let forms = self.forms();
        forms[self.row.min(forms.len() - 1)].write(&self.letters)
    }

    /// What `flush` would commit now.
    fn staged(&self) -> String {
        if self.letters.is_empty() { String::new() } else { self.shown() }
    }

    /// A reply for what shows now, after `commit`.
    fn out(&self, commit: String, consumed: bool) -> Reply {
        if self.letters.is_empty() {
            return Reply { commit, consumed, ..Reply::default() };
        }
        let shown = self.shown();
        let rows = self.forms().into_iter().enumerate().map(|(i, f)| Row {
            text: f.write(&self.letters),
            entry: i as i32,
            lang: Some("en".into()),
            ..Row::default()
        });
        Reply {
            commit,
            consumed,
            preedit: Preedit::of(&shown, if self.row == 0 { SegmentKind::Live } else { SegmentKind::Picked }),
            menu: Menu { rows: rows.collect(), selected: Some(self.row), strip: false },
            staged: shown,
            ..Reply::default()
        }
    }

    /// What shows, unstaged.
    fn take(&mut self) -> String {
        let shown = self.staged();
        self.letters.clear();
        self.row = 0;
        shown
    }

    fn step(&mut self, down: bool) -> Reply {
        let last = self.forms().len() - 1;
        self.row = if down { (self.row + 1).min(last) } else { self.row.saturating_sub(1) };
        self.out(String::new(), true)
    }
}

impl bada_compat::Engine for Shout {
    type Data = ();
    type Session = Session;

    fn describe() -> Describe {
        Describe {
            engine: EngineInfo {
                id: "com.teammirae.shout".into(),
                version: env!("CARGO_PKG_VERSION").into(),
                name: bada_compat::en("shout"),
            },
            events: ["key", "action", "select", "pick", "text", "flush", "reset", "state", "settings"]
                .map(String::from)
                .into(),
            features: vec!["segments".into()],
            keyboards: vec!["jis".into()],
            data: vec![],
            layouts: vec![],
            actions: ["menu.next", "menu.prev", "fix", "shout"].map(String::from).into(),
            settings: SettingsInfo {
                format: bada_compat::SETTINGS_FORMAT.into(),
                schema: json!({
                    "type": "object",
                    "additionalProperties": false,
                    "properties": {
                        "forms": {"title": {"en": "The menu's rows, top first"}, "type": "array",
                                  "items": {"enum": ["upper", "lower", "title"]}, "minItems": 1, "uniqueItems": true},
                        "henkan": {"type": "object", "additionalProperties": false, "properties": {
                            "profiles": {"type": "array", "maxItems": PROFILES, "items": {
                                "type": "object", "required": ["id", "name"], "additionalProperties": false,
                                "properties": {"id": {"type": "string", "pattern": "^[a-z0-9-]{1,32}$"}, "name": {"type": "string"}}}}}}
                    }
                }),
                defaults: serde_json::to_value(Settings::default()).unwrap_or_default(),
            },
            limits: Limits { profiles: PROFILES as u32 },
        }
    }

    fn data(_buffers: &[bada_compat::Buffer<'_>]) -> Option<()> {
        Some(())
    }

    fn session(_data: Option<Arc<()>>, _harness: &Harness) -> Session {
        Session { settings: RwLock::new(Settings::default()), problems: RwLock::new(Vec::new()) }
    }

    fn settings(session: &Session, change: Change, dry: bool) -> SessionReply {
        let current = || serde_json::to_value(&*session.settings.read().unwrap()).unwrap_or_default();
        let wanted = match change {
            Change::Read => {
                let problems = session.problems.read().unwrap().clone();
                return SessionReply { settings: Some(current()), problems, ..SessionReply::default() };
            }
            Change::Set(value) => value,
            Change::Patch(patch) => {
                let mut value = current();
                merge_patch(&mut value, &patch);
                value
            }
            Change::Remove(_) => Value::Null,
        };
        let (taken, problems) = match check(&wanted) {
            Ok(settings) => (Some(settings), Vec::new()),
            Err(problems) => (None, problems),
        };
        if !dry {
            *session.problems.write().unwrap() = problems.clone();
            if let Some(settings) = &taken {
                *session.settings.write().unwrap() = settings.clone();
            }
        }
        // What would be in effect: the new settings if they were good, and otherwise the old ones.
        let in_effect = taken.map(|s| serde_json::to_value(s).unwrap_or_default()).unwrap_or_else(current);
        SessionReply { settings: Some(in_effect), ..SessionReply::default() }.with_problems(problems)
    }

    fn new(session: Arc<Session>) -> Self {
        Shout { session, mode: Mode::Muhenkan, profile: None, letters: String::new(), row: 0, waiting: false }
    }

    fn key(&mut self, e: &KeyEvent) -> Result<Reply, Error> {
        // For the layer's tests of its panic guard: no keyboard has this key.
        if e.code == "ShoutPanics" {
            self.letters.push('!');
            panic!("ShoutPanics was pressed");
        }
        let key = e.key();
        let m = e.mods;
        if self.mode == Mode::Muhenkan {
            return Ok(Reply::default());
        }
        // The settings may have lost rows since the last key.
        self.row = self.row.min(self.forms().len() - 1);
        let staged = !self.letters.is_empty();
        Ok(match key {
            k if k.letter().is_some() && m.is_plain() => {
                self.letters.extend(k.letter());
                self.row = 0;
                self.out(String::new(), true)
            }
            Key::Backspace if staged && m.is_plain() => {
                self.letters.pop();
                self.row = 0;
                self.out(String::new(), true)
            }
            Key::Tab | Key::ArrowDown if staged && !m.shift => self.step(true),
            Key::Tab | Key::ArrowUp if staged => self.step(false),
            Key::Enter if staged && m.shift => {
                let commit = self.take();
                self.out(commit, true)
            }
            _ => {
                let commit = self.take();
                self.out(commit, false)
            }
        })
    }

    fn select(&mut self, mode: Mode, profile: Option<&str>) -> Result<Reply, Error> {
        if mode == Mode::Henkan {
            // Read and let go: `take` below reads the settings again.
            let profiles = self.session.settings.read().unwrap().henkan.profiles.clone();
            if profiles.is_empty() {
                return Err(Error::new(code::NO_PROFILE, "the settings have no henkan profile"));
            }
            if !profiles.iter().any(|p| Some(p.id.as_str()) == profile) {
                return Err(Error::at(
                    code::UNKNOWN_PROFILE,
                    "profile",
                    format!("no profile named {}", profile.unwrap_or("")),
                ));
            }
        }
        if mode == self.mode && profile == self.profile.as_deref() {
            return Ok(self.out(String::new(), false));
        }
        let commit = self.take();
        self.mode = mode;
        self.profile = profile.map(str::to_string);
        self.waiting = false;
        Ok(self.out(commit, false))
    }

    fn flush(&mut self, _cause: Cause) -> Reply {
        let commit = self.take();
        self.out(commit, false)
    }

    fn reset(&mut self) -> Reply {
        self.take();
        self.waiting = false;
        self.out(String::new(), false)
    }

    fn action(&mut self, name: &str) -> Result<Reply, Error> {
        match name {
            "menu.next" => Ok(self.step(true)),
            "menu.prev" => Ok(self.step(false)),
            // shout has nothing to fix: what shows stays.
            "fix" => Ok(self.out(String::new(), true)),
            "shout" if self.letters.is_empty() => {
                self.waiting = true;
                Ok(Reply { consumed: true, ask: Some(Ask::Selection("shout".into())), ..Reply::default() })
            }
            "shout" => Ok(self.out(String::new(), true)),
            other => Err(Error::new(code::UNKNOWN_ACTION, format!("no action named {other}"))),
        }
    }

    fn pick(&mut self, row: usize) -> Result<Reply, Error> {
        if self.letters.is_empty() || row >= self.forms().len() {
            return Err(Error::at(code::BAD_ROW, "row", format!("the menu has no row {row}")));
        }
        self.row = row;
        Ok(self.out(String::new(), true))
    }

    fn text(&mut self, text: &Text) -> Reply {
        let waiting = std::mem::take(&mut self.waiting);
        match text.selection.as_deref() {
            Some(selected) if waiting && !selected.is_empty() => Reply {
                commit: selected.to_uppercase(),
                replace: Replace::Selection,
                consumed: true,
                ..Reply::default()
            },
            _ => self.out(String::new(), true),
        }
    }
}

bada_compat::export! {
    prefix: "shout",
    engine: Shout,
}
