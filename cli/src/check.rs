//! What is wrong with a settings file, a layout or a dictionary (the standard, §6.6, §10.1). The
//! engine checks its own parts on a session made for the check, which has the folder's layouts
//! and is thrown away: nothing here changes a file or a running input method.

use std::ffi::CStr;

use bada::Session;
use bada_compat::{Change, Harness, LayoutInfo, Level, Problem};
use serde_json::{Value, json};

use crate::folder::Folder;
use crate::keys;

/// The answer to a check: the problems, with paths into the file, and the session's layouts.
#[derive(Debug, Default)]
pub struct Checked {
    pub problems: Vec<Problem>,
    /// As `P_describe.layouts` has them, the user's own included.
    pub layouts: Vec<LayoutInfo>,
}

impl Checked {
    /// No error-level problem: the file can be written.
    pub fn ok(&self) -> bool {
        self.problems.iter().all(|p| p.level != Level::Error)
    }

    pub fn json(&self) -> Value {
        json!({"ok": self.ok(), "problems": self.problems, "layouts": self.layouts})
    }

    fn only(problem: Problem, layouts: Vec<LayoutInfo>) -> Checked {
        Checked { problems: vec![problem], layouts }
    }
}

/// The engine's `P_describe` answer, as a harness reads it.
pub fn describe() -> &'static str {
    // Static JSON, valid for the life of the program (the standard, §3.2).
    unsafe { CStr::from_ptr(bada::ffi::describe()) }.to_str().unwrap_or("{}")
}

/// A session with the folder's own layouts, but for `skip`. A layout that can't be read is left
/// out, as a harness leaves it out.
fn session(folder: &Folder, skip: Option<&str>) -> Session {
    let harness = Harness { id: crate::CLI_ID.into(), version: crate::VERSION.into(), features: vec![] };
    let session = Session::new(&harness, None);
    for (id, text) in folder.layouts() {
        if skip == Some(id.as_str()) {
            continue;
        }
        if let Ok(value) = serde_json::from_str::<Value>(&text) {
            session.layout(Change::Set(value), false);
        }
    }
    session
}

fn layouts(session: &Session) -> Vec<LayoutInfo> {
    session.layout(Change::Read, false).layouts.unwrap_or_default()
}

fn not_json(error: &serde_json::Error) -> Problem {
    Problem::error("", "bad-json", format!("not JSON: {error}"))
}

/// `settings.json`, as one file: its `engine` part by the engine, its `keys` here, and the rest by
/// the format (§10.1).
pub fn settings(folder: &Folder, text: &str) -> Checked {
    let session = session(folder, None);
    let layouts = layouts(&session);
    let file: Value = match serde_json::from_str(text) {
        Ok(file) => file,
        Err(e) => return Checked::only(not_json(&e), layouts),
    };
    let Some(parts) = file.as_object() else {
        return Checked::only(Problem::error("", "bad-value", "a settings file is an object"), layouts);
    };
    let mut problems = Vec::new();
    if parts.get("format").and_then(Value::as_str) != Some(bada_compat::SETTINGS_FORMAT) {
        let message = format!("a settings file's format is {}", bada_compat::SETTINGS_FORMAT);
        problems.push(Problem::error("format", "bad-format", message));
    }
    for name in parts.keys().filter(|name| !["format", "engine", "keys", "harness"].contains(&name.as_str())) {
        problems.push(Problem::error(name, "unknown-field", format!("a settings file has no part {name}")));
    }
    if let Some(engine) = parts.get("engine") {
        for mut problem in session.settings(Change::Set(engine.clone()), true).problems {
            problem.path = if problem.path.is_empty() { "engine".into() } else { format!("engine.{}", problem.path) };
            problems.push(problem);
        }
    }
    let profiles: Vec<String> = file
        .pointer("/engine/henkan/profiles")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(|p| p.get("id").and_then(Value::as_str).map(String::from))
        .collect();
    problems.extend(keys::check(parts.get("keys"), &profiles, &layouts));
    if let Some(harness) = parts.get("harness") {
        match harness.as_object() {
            None => problems.push(Problem::error("harness", "bad-value", "harness is an object, by harness id")),
            Some(own) => {
                for (id, part) in own.iter().filter(|(_, part)| !part.is_object()) {
                    let message = format!("a harness's own settings are an object, not {part}");
                    problems.push(Problem::error(&format!("harness.{id}"), "bad-value", message));
                }
            }
        }
    }
    Checked { problems, layouts }
}

/// A layout's file: a dry `layout` event on a session that has the folder's other layouts.
/// Also the file's own id, when it has one.
pub fn layout(folder: &Folder, text: &str) -> (Checked, Option<String>) {
    let value: Value = match serde_json::from_str(text) {
        Ok(value) => value,
        Err(e) => {
            let session = session(folder, None);
            return (Checked::only(not_json(&e), layouts(&session)), None);
        }
    };
    let id = value.get("id").and_then(Value::as_str).map(String::from);
    let session = session(folder, id.as_deref());
    let reply = session.layout(Change::Set(value), true);
    let layouts = reply.layouts.unwrap_or_else(|| layouts(&session));
    (Checked { problems: reply.problems, layouts }, id)
}

/// The user dictionary (§11): the `dictionary` event's problems.
pub fn dictionary(folder: &Folder, text: &str) -> Checked {
    let session = session(folder, None);
    let layouts = layouts(&session);
    match serde_json::from_str::<Value>(text) {
        Ok(value) => Checked { problems: session.dictionary(Change::Set(value)).problems, layouts },
        Err(e) => Checked::only(not_json(&e), layouts),
    }
}
