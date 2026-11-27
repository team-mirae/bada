//! What every command answers, and reading and writing `settings.json` for the commands that change
//! it (the standard, §10.2). A change is checked as the page's host checks it, and written only
//! when it has no error-level problem; every write keeps the file it replaced in `history/`.

use bada_compat::{Level, Problem};
use serde_json::{Value, json};

use crate::folder::Folder;

/// A command's answer: `--json` prints `value` (with `ok` and `problems` in it), the rest prints
/// `text` and the problems. Not ok: exit code 1.
#[derive(Debug)]
pub struct Answer {
    pub ok: bool,
    pub value: Value,
    pub text: String,
    pub problems: Vec<Problem>,
}

impl Answer {
    pub fn ok(value: Value, text: impl Into<String>) -> Answer {
        Answer { ok: true, value, text: text.into(), problems: vec![] }
    }

    pub fn failed(code: &str, message: impl Into<String>) -> Answer {
        let message = message.into();
        Answer { ok: false, value: Value::Null, text: String::new(), problems: vec![Problem::error("", code, message)] }
    }

    pub fn with_problems(mut self, problems: Vec<Problem>) -> Answer {
        if problems.iter().any(|p| p.level == Level::Error) {
            self.ok = false;
        }
        self.problems.extend(problems);
        self
    }

    /// The JSON printed with `--json`: always an object with `ok` and `problems`.
    pub fn json(&self) -> Value {
        let mut out = match &self.value {
            Value::Object(o) => Value::Object(o.clone()),
            Value::Null => json!({}),
            other => json!({"value": other}),
        };
        out["ok"] = json!(self.ok);
        out["problems"] = json!(self.problems);
        out
    }

    /// The text printed without `--json`.
    pub fn render(&self) -> String {
        let mut out = self.text.clone();
        for p in &self.problems {
            let level = if p.level == Level::Error { "error" } else { "warning" };
            let at = if p.path.is_empty() { String::new() } else { format!(" {}", p.path) };
            if !out.is_empty() && !out.ends_with('\n') {
                out.push('\n');
            }
            out.push_str(&format!("{level}{at} {}: {}", p.code, p.message));
        }
        out
    }
}

/// The settings file as it is, or a new one when there is none (a new install).
pub fn load(folder: &Folder) -> Result<Value, Answer> {
    let path = folder.dir.join("settings.json");
    match std::fs::read_to_string(&path) {
        Ok(text) => serde_json::from_str(&text).map_err(|e| Answer::failed("bad-json", format!("settings.json isn't JSON: {e}"))),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(json!({"format": bada_compat::SETTINGS_FORMAT})),
        Err(e) => Err(Answer::failed("not-read", format!("{}: {e}", path.display()))),
    }
}

/// Writes the settings file, if it checks: the answer says what was done, and what is wrong.
pub fn save(folder: &Folder, settings: &Value, done: &str) -> Answer {
    let text = serde_json::to_string_pretty(settings).unwrap_or_default() + "\n";
    let outcome = folder.write("settings.json", &text);
    let mut answer = if outcome.ok {
        Answer::ok(json!({"written": "settings.json"}), done)
    } else {
        Answer { ok: false, value: json!({"written": null}), text: "Nothing was written.".into(), problems: vec![] }
    };
    answer.problems = outcome.problems;
    answer
}

/// `engine.henkan.profiles`, as a list (empty when there is none).
pub fn profiles(settings: &Value) -> Vec<Value> {
    settings.pointer("/engine/henkan/profiles").and_then(Value::as_array).cloned().unwrap_or_default()
}

pub fn set_profiles(settings: &mut Value, profiles: Vec<Value>) {
    crate::path::set(settings, &crate::path::parse("engine.henkan.profiles").unwrap(), Value::Array(profiles)).ok();
}
