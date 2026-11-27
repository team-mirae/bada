//! `bada type --profile <id> "<keys>"` (the standard, §10.2): types on the engine with no text field
//! and prints the result, to test a profile or a custom layout. The settings folder's layouts,
//! settings and dictionary are loaded as a harness loads them; nothing is written.
//!
//! The keys: each character is typed as on a JIS keyboard (`a`, `A` with ⇧, `!` as ⇧1, a space as
//! Space, a newline as Enter); `{Code}` sends a key by its `KeyboardEvent.code` (`{Backspace}`,
//! `{Enter}`, `{F7}`), with modifiers as `{shift+Code}`, `{alt+shift+Digit1}`; `{action:menu.next}`
//! runs an action and `{pick:2}` picks a menu row. `{{` and `}}` type a brace.

use std::sync::Arc;

use bada::layout::jis;
use bada::{Data, Field, Session};
use bada_compat::{Buffer, Cause, Change, Harness, Key, KeyEvent, Mode, Mods, Reply};
use serde_json::{Value, json};

use crate::edit::{self, Answer};
use crate::folder::Folder;

/// One step of the keys.
#[derive(Debug, PartialEq)]
pub enum Step {
    Key(String, Mods),
    Action(String),
    Pick(usize),
}

/// Reads the keys; `Err` names what isn't one.
pub fn parse(keys: &str) -> Result<Vec<Step>, String> {
    let mut steps = Vec::new();
    let mut chars = keys.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '{' && chars.peek() == Some(&'{') {
            chars.next();
            steps.push(char_step('{')?);
        } else if c == '}' && chars.peek() == Some(&'}') {
            chars.next();
            steps.push(char_step('}')?);
        } else if c == '{' {
            let mut inner = String::new();
            loop {
                match chars.next() {
                    Some('}') => break,
                    Some(ch) => inner.push(ch),
                    None => return Err(format!("{{{inner} has no closing }}")),
                }
            }
            steps.push(braced(&inner)?);
        } else {
            steps.push(char_step(c)?);
        }
    }
    Ok(steps)
}

fn braced(inner: &str) -> Result<Step, String> {
    if let Some(name) = inner.strip_prefix("action:") {
        return Ok(Step::Action(name.to_string()));
    }
    if let Some(row) = inner.strip_prefix("pick:") {
        return row.parse().map(Step::Pick).map_err(|_| format!("{{pick:{row}}}: a row is a number"));
    }
    let mut parts: Vec<&str> = inner.split('+').collect();
    let code = parts.pop().filter(|c| !c.is_empty()).ok_or_else(|| format!("{{{inner}}} names no key"))?;
    let mut mods = Mods::NONE;
    for p in parts {
        match p {
            "shift" => mods.shift = true,
            "ctrl" => mods.ctrl = true,
            "alt" => mods.alt = true,
            "meta" => mods.meta = true,
            _ => return Err(format!("{{{inner}}}: {p} isn't a modifier (ctrl, alt, shift, meta)")),
        }
    }
    if Key::from_code(code) == Key::Other {
        return Err(format!("{{{inner}}}: {code} isn't a key code (KeyA, Digit1, Enter, …)"));
    }
    Ok(Step::Key(code.to_string(), mods))
}

/// A character, as the JIS key that types it (with ⇧ when that's where it is).
fn char_step(c: char) -> Result<Step, String> {
    match c {
        ' ' => return Ok(Step::Key("Space".into(), Mods::NONE)),
        '\n' => return Ok(Step::Key("Enter".into(), Mods::NONE)),
        _ => {}
    }
    for &key in Key::ALL {
        for shift in [false, true] {
            if jis(key, shift) == Some(c) {
                return Ok(Step::Key(key.code().into(), if shift { Mods::SHIFT } else { Mods::NONE }));
            }
        }
    }
    Err(format!("{c:?} has no key on a JIS keyboard: write it as {{Code}}"))
}

/// The engine's data, from the first folder that has it: `$BADA_DATA`, beside the binary (`data`,
/// the Mac bundle's `../Resources/data`, `../share/bada/data`), or the repository's `engine/data`.
fn data() -> Option<Arc<Data>> {
    let mut folders = Vec::new();
    if let Ok(d) = std::env::var("BADA_DATA") {
        folders.push(std::path::PathBuf::from(d));
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            folders.extend([dir.join("data"), dir.join("../Resources/data"), dir.join("../share/bada/data")]);
        }
    }
    folders.push(std::path::PathBuf::from(concat!(env!("CARGO_MANIFEST_DIR"), "/../engine/data")));
    let folder = folders.into_iter().find(|f| f.is_dir())?;
    let files: Vec<(String, Vec<u8>)> = bada::BUFFERS
        .iter()
        .filter_map(|(name, _)| {
            let entry = std::fs::read_dir(&folder).ok()?.flatten().find(|e| e.path().file_stem().and_then(|s| s.to_str()) == Some(name))?;
            Some((name.to_string(), std::fs::read(entry.path()).ok()?))
        })
        .collect();
    let buffers: Vec<Buffer<'_>> = files.iter().map(|(n, b)| Buffer { name: n, bytes: b, borrowed: false }).collect();
    Some(Arc::new(Data::new(&buffers)))
}

/// `type`: the text an app would have after the keys, what is still staged, and the menu.
pub fn run(folder: &Folder, profile: Option<&str>, keys: &str) -> Answer {
    let steps = match parse(keys) {
        Ok(s) => s,
        Err(e) => return Answer::failed("bad-keys", e),
    };
    let harness = Harness { id: crate::CLI_ID.into(), version: crate::VERSION.into(), features: vec![] };
    let session = Arc::new(Session::new(&harness, data()));
    let mut problems = Vec::new();
    for (_, text) in folder.layouts() {
        if let Ok(v) = serde_json::from_str::<Value>(&text) {
            problems.extend(session.layout(Change::Set(v), false).problems);
        }
    }
    let settings = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    if let Some(engine) = settings.get("engine") {
        problems.extend(session.settings(Change::Set(engine.clone()), false).problems);
    }
    if let Ok(t) = std::fs::read_to_string(folder.dir.join("dictionary.json")) {
        if let Ok(v) = serde_json::from_str::<Value>(&t) {
            problems.extend(session.dictionary(Change::Set(v)).problems);
        }
    }
    let mut field = Field::new(session);
    let (mode, label) = match profile {
        Some(p) => (Mode::Henkan, format!("henkan, profile {p}")),
        None => (Mode::Muhenkan, "muhenkan".to_string()),
    };
    if let Err(e) = field.select(mode, profile) {
        return Answer::failed(&e.code, e.message).with_problems(problems);
    }
    let mut text = String::new();
    let mut last = Reply::default();
    let mut passed = Vec::new();
    for step in &steps {
        let reply = match step {
            Step::Key(code, mods) => field.key(&KeyEvent::new(code, *mods)),
            Step::Action(name) => field.action(name),
            Step::Pick(row) => field.pick(*row),
        };
        let reply = match reply {
            Ok(r) => r,
            Err(e) => return Answer::failed(&e.code, e.message).with_problems(problems),
        };
        text.push_str(&reply.commit);
        if !reply.consumed {
            if let Step::Key(code, _) = step {
                match (code.as_str(), &reply.types) {
                    (_, Some(t)) => text.push_str(t),
                    ("Backspace", None) => {
                        text.pop();
                    }
                    ("Enter", None) => text.push('\n'),
                    _ => passed.push(code.clone()),
                }
            }
        }
        last = reply;
    }
    let staged = last.preedit.text.clone();
    let menu: Vec<&str> = last.menu.rows.iter().map(|r| r.text.as_str()).collect();
    let flushed = format!("{text}{}", field.flush(Cause::Harness).commit);
    let mut out = text.clone();
    if !staged.is_empty() {
        out.push_str(&format!("[{staged}]"));
    }
    if !menu.is_empty() {
        out.push_str(&format!("\nmenu: {}", menu.join(" | ")));
    }
    if !passed.is_empty() {
        out.push_str(&format!("\npassed on: {}", passed.join(" ")));
    }
    Answer::ok(
        json!({"mode": label, "text": text, "staged": staged, "menu": menu, "selected": last.menu.selected,
               "flushed": flushed, "passed": passed}),
        out,
    )
    // Loading the folder's layouts warns as a harness would (a new id has no key model): only
    // errors matter to a test run.
    .with_problems(problems.into_iter().filter(|p| p.level == bada_compat::Level::Error).collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keys_read_as_jis() {
        assert_eq!(parse("aA").unwrap(), vec![Step::Key("KeyA".into(), Mods::NONE), Step::Key("KeyA".into(), Mods::SHIFT)]);
        assert_eq!(parse("!@").unwrap(), vec![Step::Key("Digit1".into(), Mods::SHIFT), Step::Key("BracketLeft".into(), Mods::NONE)]);
        assert_eq!(parse("{Backspace}{alt+shift+Digit1}").unwrap()[1], Step::Key("Digit1".into(), Mods { shift: true, ctrl: false, alt: true, meta: false }));
        assert_eq!(parse("{action:menu.next}{pick:2}").unwrap(), vec![Step::Action("menu.next".into()), Step::Pick(2)]);
        assert_eq!(parse("{{}}").unwrap(), vec![Step::Key("BracketRight".into(), Mods::SHIFT), Step::Key("Backslash".into(), Mods::SHIFT)]);
        assert!(parse("{Nope}").is_err() && parse("{KeyA").is_err() && parse("é").is_err());
    }
}
