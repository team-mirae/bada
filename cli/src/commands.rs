//! The command line's commands on the settings folder (the standard, §10.2): `settings`, `profile`,
//! `keys`, `dict`, `schema`. Each answers an `Answer`; nothing here asks a question.

use bada_compat::Problem;
use serde_json::{Value, json};

use crate::edit::{self, Answer};
use crate::folder::Folder;
use crate::keys::{On, is_action, is_layout_id, is_profile_id};
use crate::path;

// MARK: settings

pub fn settings_show(folder: &Folder) -> Answer {
    match edit::load(folder) {
        Ok(s) => Answer::ok(json!({"settings": s}), serde_json::to_string_pretty(&s).unwrap_or_default()),
        Err(a) => a,
    }
}

pub fn settings_get(folder: &Folder, at: &str) -> Answer {
    let Some(steps) = path::parse(at) else { return Answer::failed("bad-path", format!("{at:?} isn't a path")) };
    let s = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    match path::get(&s, &steps) {
        Some(v) => Answer::ok(json!({"path": at, "value": v}), serde_json::to_string_pretty(v).unwrap_or_default()),
        None => Answer::failed("no-value", format!("settings.json has nothing at {}", path::shown(&steps))),
    }
}

pub fn settings_set(folder: &Folder, at: &str, value: &str) -> Answer {
    let Some(steps) = path::parse(at) else { return Answer::failed("bad-path", format!("{at:?} isn't a path")) };
    let Ok(value) = serde_json::from_str::<Value>(value) else {
        return Answer::failed("bad-json", format!("{value:?} isn't JSON (a string is written in quotes: '\"ko\"')"));
    };
    let mut s = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    if let Err(e) = path::set(&mut s, &steps, value) {
        return Answer::failed("bad-path", e);
    }
    edit::save(folder, &s, &format!("Set {}.", path::shown(&steps)))
}

pub fn settings_import(folder: &Folder, file: &str) -> Answer {
    let text = match std::fs::read_to_string(file) {
        Ok(t) => t,
        Err(e) => return Answer::failed("not-read", format!("{file}: {e}")),
    };
    let outcome = folder.write("settings.json", &text);
    let mut a = if outcome.ok {
        Answer::ok(json!({"written": "settings.json"}), format!("Imported {file} as settings.json."))
    } else {
        Answer { ok: false, value: json!({"written": null}), text: "Nothing was written.".into(), problems: vec![] }
    };
    a.problems = outcome.problems;
    a
}

/// `settings export`: the file as it is (a new install's when there is none), for another machine.
pub fn settings_export(folder: &Folder) -> Answer {
    settings_show(folder)
}

/// `settings undo`: back to the file the last write replaced (`history/`, §10.1). The file it
/// replaces goes into the history in turn, so a second undo goes forward again.
pub fn settings_undo(folder: &Folder) -> Answer {
    let Some(last) = folder.history().into_iter().max() else {
        return Answer::failed("no-history", "there is no earlier settings.json to go back to");
    };
    let outcome = folder.undo(&last);
    let mut a = if outcome.ok {
        Answer::ok(json!({"restored": last}), format!("settings.json is {last} again."))
    } else {
        Answer { ok: false, value: json!({"restored": null}), text: "Nothing was written.".into(), problems: vec![] }
    };
    a.problems = outcome.problems;
    a
}

// MARK: profile (§8)

fn find(profiles: &[Value], id: &str) -> Option<usize> {
    profiles.iter().position(|p| p.get("id").and_then(Value::as_str) == Some(id))
}

pub fn profile_list(folder: &Folder) -> Answer {
    let s = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    let profiles = edit::profiles(&s);
    let mut text = String::new();
    for p in &profiles {
        let id = p.get("id").and_then(Value::as_str).unwrap_or("?");
        let name = p.get("name").and_then(Value::as_str).unwrap_or("");
        let layouts: Vec<&str> = p.get("layouts").and_then(Value::as_array).into_iter().flatten().filter_map(Value::as_str).collect();
        text.push_str(&format!("{id}\t{name}\t{}\n", layouts.join(" ")));
    }
    if profiles.is_empty() {
        text = "No henkan profiles.".into();
    }
    Answer::ok(json!({"profiles": profiles}), text.trim_end().to_string())
}

/// `profile add <id> --layouts a,b [--name <name>]`.
pub fn profile_add(folder: &Folder, id: &str, layouts: &[String], name: Option<&str>) -> Answer {
    if !is_profile_id(id) {
        return Answer::failed("bad-value", format!("{id:?} isn't a profile id (lower-case letters, digits and -, up to 32)"));
    }
    if layouts.is_empty() {
        return Answer::failed("bad-value", "a profile has at least one layout: --layouts a,b");
    }
    if let Some(bad) = layouts.iter().find(|l| !is_layout_id(l)) {
        return Answer::failed("bad-value", format!("{bad:?} isn't a layout id"));
    }
    let mut s = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    let mut profiles = edit::profiles(&s);
    if find(&profiles, id).is_some() {
        return Answer::failed("duplicate-profile", format!("there is a profile {id} already: `bada profile set` changes it"));
    }
    let mut p = json!({"id": id, "name": name.unwrap_or(id), "layouts": layouts});
    if name.is_none() {
        p["name"] = json!(id);
    }
    profiles.push(p);
    edit::set_profiles(&mut s, profiles);
    edit::save(folder, &s, &format!("Added profile {id}."))
}

pub fn profile_remove(folder: &Folder, id: &str) -> Answer {
    let mut s = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    let mut profiles = edit::profiles(&s);
    let Some(i) = find(&profiles, id) else { return Answer::failed("unknown-profile", format!("there is no profile {id}")) };
    profiles.remove(i);
    edit::set_profiles(&mut s, profiles);
    // The key table may still name it: the check says so (a warning, §10.1).
    edit::save(folder, &s, &format!("Removed profile {id}."))
}

/// `profile move <id> <n>`: to place n of the list, from 0.
pub fn profile_move(folder: &Folder, id: &str, to: usize) -> Answer {
    let mut s = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    let mut profiles = edit::profiles(&s);
    let Some(i) = find(&profiles, id) else { return Answer::failed("unknown-profile", format!("there is no profile {id}")) };
    if to >= profiles.len() {
        return Answer::failed("bad-value", format!("there are {} profiles: places 0 to {}", profiles.len(), profiles.len() - 1));
    }
    let p = profiles.remove(i);
    profiles.insert(to, p);
    edit::set_profiles(&mut s, profiles);
    edit::save(folder, &s, &format!("Moved profile {id} to place {to}."))
}

/// `profile set <id> <field> <json>`: one field of a profile (`name`, `layouts`, `script_forms`, …).
pub fn profile_set(folder: &Folder, id: &str, field: &str, value: &str) -> Answer {
    let Ok(value) = serde_json::from_str::<Value>(value) else {
        return Answer::failed("bad-json", format!("{value:?} isn't JSON"));
    };
    if field == "id" {
        return Answer::failed("bad-value", "a profile's id doesn't change: remove it and add another");
    }
    let mut s = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    let mut profiles = edit::profiles(&s);
    let Some(i) = find(&profiles, id) else { return Answer::failed("unknown-profile", format!("there is no profile {id}")) };
    let Some(p) = profiles[i].as_object_mut() else { return Answer::failed("bad-value", "the profile isn't an object") };
    if value.is_null() {
        p.remove(field);
    } else {
        p.insert(field.to_string(), value);
    }
    edit::set_profiles(&mut s, profiles);
    edit::save(folder, &s, &format!("Set {field} of profile {id}."))
}

// MARK: keys (§9)

fn bindings(s: &Value) -> Vec<Value> {
    s.pointer("/keys/bindings").and_then(Value::as_array).cloned().unwrap_or_default()
}

pub fn keys_list(folder: &Folder) -> Answer {
    let s = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    let keys = s.get("keys").cloned().unwrap_or(json!({}));
    let mut text = String::new();
    for b in bindings(&s) {
        let on = b.get("on").and_then(Value::as_str).unwrap_or("?");
        let actions: Vec<&str> = b.get("do").and_then(Value::as_array).into_iter().flatten().filter_map(Value::as_str).collect();
        let when = b.get("when").and_then(Value::as_str).map(|w| format!("  (in {w})")).unwrap_or_default();
        text.push_str(&format!("{on}\t{}{when}\n", actions.join(", ")));
    }
    for field in ["cycle", "henkan_enters", "caps_lock", "caps_rule"] {
        if let Some(v) = keys.get(field) {
            text.push_str(&format!("{field}: {v}\n"));
        }
    }
    if text.is_empty() {
        text = "No bindings: the harness's defaults (§9.5).".into();
    }
    Answer::ok(json!({"keys": keys}), text.trim_end().to_string())
}

/// `keys bind <key> <action>… [--when henkan|muhenkan]`: replaces the binding on that key (and
/// `when`), or adds one.
pub fn keys_bind(folder: &Folder, on: &str, actions: &[String], when: Option<&str>) -> Answer {
    let Some(parsed) = On::parse(on) else {
        return Answer::failed("bad-value", format!("{on:?} isn't a key: a code (Lang1), with modifiers (shift+Lang1), or two language keys (KanaMode+Lang1)"));
    };
    if actions.is_empty() {
        return Answer::failed("bad-value", "a binding does at least one action");
    }
    if let Some(bad) = actions.iter().find(|a| !is_action(a)) {
        return Answer::failed("unknown-action", format!("{bad:?} isn't an action (§9.2)"));
    }
    if let Some(w) = when.filter(|w| !["henkan", "muhenkan"].contains(w)) {
        return Answer::failed("bad-value", format!("when is henkan or muhenkan, not {w:?}"));
    }
    let name = parsed.name();
    let mut s = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    let mut list = bindings(&s);
    let mut b = json!({"on": name, "do": actions});
    if let Some(w) = when {
        b["when"] = json!(w);
    }
    let same = |x: &Value| {
        x.get("on").and_then(Value::as_str).and_then(On::parse).map(|o| o.name()) == Some(name.clone())
            && x.get("when").and_then(Value::as_str) == when
    };
    match list.iter().position(same) {
        Some(i) => list[i] = b,
        None => list.push(b),
    }
    path::set(&mut s, &path::parse("keys.bindings").unwrap(), Value::Array(list)).ok();
    edit::save(folder, &s, &format!("{name} does {}.", actions.join(", ")))
}

pub fn keys_unbind(folder: &Folder, on: &str, when: Option<&str>) -> Answer {
    let Some(parsed) = On::parse(on) else { return Answer::failed("bad-value", format!("{on:?} isn't a key")) };
    let name = parsed.name();
    let mut s = match edit::load(folder) {
        Ok(s) => s,
        Err(a) => return a,
    };
    let list = bindings(&s);
    let before = list.len();
    let kept: Vec<Value> = list
        .into_iter()
        .filter(|x| {
            !(x.get("on").and_then(Value::as_str).and_then(On::parse).map(|o| o.name()) == Some(name.clone())
                && (when.is_none() || x.get("when").and_then(Value::as_str) == when))
        })
        .collect();
    if kept.len() == before {
        return Answer::failed("no-binding", format!("nothing is bound on {name}"));
    }
    path::set(&mut s, &path::parse("keys.bindings").unwrap(), Value::Array(kept)).ok();
    edit::save(folder, &s, &format!("{name} is unbound."))
}

/// `keys available`: what the running harness detects now (its published state, §6.7).
pub fn keys_available(folder: &Folder) -> Answer {
    let state = crate::state::read(folder);
    let keys = state.pointer("/harness/keys").cloned().unwrap_or(Value::Null);
    if keys.is_null() {
        return Answer::ok(json!({"keys": null}), "No input method is running: nothing is published.");
    }
    let list = |k: &str| keys.get(k).and_then(Value::as_array).map(|a| a.iter().filter_map(Value::as_str).collect::<Vec<_>>().join(" ")).unwrap_or_default();
    let text = format!(
        "available: {}\nchords: {}\nmissing: {}",
        list("available"),
        keys.get("chords").and_then(Value::as_bool).unwrap_or(false),
        if list("missing").is_empty() { "nothing".into() } else { list("missing") }
    );
    Answer::ok(json!({"keys": keys}), text)
}

// MARK: dict (§11)

fn load_dict(folder: &Folder) -> Result<Value, Answer> {
    match std::fs::read_to_string(folder.dir.join("dictionary.json")) {
        Ok(t) => serde_json::from_str(&t).map_err(|e| Answer::failed("bad-json", format!("dictionary.json isn't JSON: {e}"))),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(json!({"format": "bada-dictionary", "version": 1, "positive": [], "negative": []})),
        Err(e) => Err(Answer::failed("not-read", e.to_string())),
    }
}

fn save_dict(folder: &Folder, dict: &Value, done: &str) -> Answer {
    let text = serde_json::to_string_pretty(dict).unwrap_or_default() + "\n";
    let outcome = folder.write("dictionary.json", &text);
    let mut a = if outcome.ok {
        Answer::ok(json!({"written": "dictionary.json"}), done)
    } else {
        Answer { ok: false, value: json!({"written": null}), text: "Nothing was written.".into(), problems: vec![] }
    };
    a.problems = outcome.problems;
    a
}

pub fn dict_show(folder: &Folder) -> Answer {
    match load_dict(folder) {
        Ok(d) => {
            let words = |k: &str| d.get(k).and_then(Value::as_array).map(|a| a.iter().filter_map(|w| w.get("word").and_then(Value::as_str)).collect::<Vec<_>>().join(" ")).unwrap_or_default();
            Answer::ok(json!({"dictionary": d}), format!("positive: {}\nnegative: {}", words("positive"), words("negative")))
        }
        Err(a) => a,
    }
}

/// `dict add <word> [--negative] [--language korean|english|japanese]`.
pub fn dict_add(folder: &Folder, word: &str, negative: bool, language: Option<&str>) -> Answer {
    if word.is_empty() {
        return Answer::failed("bad-value", "a word has at least one character");
    }
    let mut d = match load_dict(folder) {
        Ok(d) => d,
        Err(a) => return a,
    };
    let (list, other) = if negative { ("negative", "positive") } else { ("positive", "negative") };
    // A word is on one side: adding it to one takes it off the other.
    if let Some(a) = d.get_mut(other).and_then(Value::as_array_mut) {
        a.retain(|w| w.get("word").and_then(Value::as_str) != Some(word));
    }
    let entries = d[list].as_array_mut().map(std::mem::take).unwrap_or_default();
    let mut entries: Vec<Value> = entries.into_iter().filter(|w| w.get("word").and_then(Value::as_str) != Some(word)).collect();
    let mut e = json!({"word": word});
    if let Some(l) = language {
        e["language"] = json!(l);
    }
    entries.push(e);
    d[list] = Value::Array(entries);
    save_dict(folder, &d, &format!("{word} is a {list} word."))
}

pub fn dict_remove(folder: &Folder, word: &str) -> Answer {
    let mut d = match load_dict(folder) {
        Ok(d) => d,
        Err(a) => return a,
    };
    let mut found = false;
    for list in ["positive", "negative"] {
        if let Some(a) = d.get_mut(list).and_then(Value::as_array_mut) {
            let before = a.len();
            a.retain(|w| w.get("word").and_then(Value::as_str) != Some(word));
            found |= a.len() != before;
        }
    }
    if !found {
        return Answer::failed("no-word", format!("{word} isn't in the dictionary"));
    }
    save_dict(folder, &d, &format!("Removed {word}."))
}

/// `dict import <file>`: a whole `bada-dictionary` file, checked, in place of the one there is.
pub fn dict_import(folder: &Folder, file: &str) -> Answer {
    let text = match std::fs::read_to_string(file) {
        Ok(t) => t,
        Err(e) => return Answer::failed("not-read", format!("{file}: {e}")),
    };
    let outcome = folder.write("dictionary.json", &text);
    let mut a = if outcome.ok {
        Answer::ok(json!({"written": "dictionary.json"}), format!("Imported {file} as dictionary.json."))
    } else {
        Answer { ok: false, value: json!({"written": null}), text: "Nothing was written.".into(), problems: vec![] }
    };
    a.problems = outcome.problems;
    a
}

// MARK: schema

pub const SETTINGS_SCHEMA: &str = include_str!("../../docs/schemas/settings.schema.json");
pub const LAYOUT_SCHEMA: &str = include_str!("../../docs/schemas/layout.schema.json");

/// `schema [settings|layout|keys]`: the JSON Schema (`keys` is the settings schema's `keys` part).
pub fn schema(which: &str) -> Answer {
    let parse = |s: &str| serde_json::from_str::<Value>(s).unwrap_or(Value::Null);
    let value = match which {
        "settings" => parse(SETTINGS_SCHEMA),
        "layout" => parse(LAYOUT_SCHEMA),
        "keys" => {
            let s = parse(SETTINGS_SCHEMA);
            let mut keys = s.pointer("/properties/keys").cloned().unwrap_or(Value::Null);
            // The part refers to the whole file's definitions.
            if let (Some(defs), Some(k)) = (s.get("$defs"), keys.as_object_mut()) {
                k.insert("$defs".into(), defs.clone());
            }
            keys
        }
        other => return Answer::failed("bad-value", format!("a schema is settings, layout or keys, not {other:?}")),
    };
    Answer::ok(json!({"schema": value}), serde_json::to_string_pretty(&value).unwrap_or_default())
}

/// A warning, for text and JSON alike.
pub fn warning(code: &str, message: impl Into<String>) -> Problem {
    Problem::warning("", code, message)
}
