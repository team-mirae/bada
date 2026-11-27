//! `bada layout …` (the standard, §10.2): layouts, stock and the user's. A change to a stock layout is
//! refused without `--modify-stock`, and the answer says why (§7.7); with it, and for any new id,
//! the warning is in the output.

use bada::layout::{self, Layer, Layout, Press, STOCK};
use bada_compat::{Key, Problem};
use serde_json::{Value, json};

use crate::check;
use crate::edit::Answer;
use crate::folder::Folder;
use crate::keys::is_layout_id;
use crate::path;

const WHY_STOCK: &str = "bada's key models are trained on the stock layouts and stored under their ids: a changed stock \
layout keeps its model, which still believes the stock keys, so in a profile with other layouts the wrong reading may show \
(§7.7). --modify-stock changes it anyway; `bada layout copy` makes a copy under a new id instead";
const WHY_NEW: &str = "a layout with a new id has no key model: among other layouts in a profile it is ranked by its rank \
scores alone (§7.7). Used alone in a profile it is unaffected";

fn stock_text(id: &str) -> Option<&'static str> {
    STOCK.iter().find(|(s, _)| *s == id).map(|(_, t)| *t)
}

fn is_stock(id: &str) -> bool {
    stock_text(id).is_some()
}

fn user_text(folder: &Folder, id: &str) -> Option<String> {
    folder.layouts().into_iter().find(|(i, _)| i == id).map(|(_, t)| t)
}

/// The layout as it is used now: the user's file, or the stock one.
fn current(folder: &Folder, id: &str) -> Option<Value> {
    user_text(folder, id).or_else(|| stock_text(id).map(String::from)).and_then(|t| serde_json::from_str(&t).ok())
}

pub fn list(folder: &Folder) -> Answer {
    let user: Vec<String> = folder.layouts().into_iter().map(|(id, _)| id).collect();
    let mut rows = Vec::new();
    for (id, text) in STOCK {
        let name = serde_json::from_str::<Value>(text).ok().and_then(|v| v.pointer("/name/en").and_then(Value::as_str).map(String::from)).unwrap_or_default();
        rows.push(json!({"id": id, "name": name, "stock": true, "modified": user.contains(&id.to_string())}));
    }
    for id in user.iter().filter(|id| !is_stock(id)) {
        let name = current(folder, id).and_then(|v| v.pointer("/name/en").and_then(Value::as_str).map(String::from)).unwrap_or_default();
        rows.push(json!({"id": id, "name": name, "stock": false, "modified": false}));
    }
    let text = rows
        .iter()
        .map(|r| {
            let mark = if r["modified"] == true { "  (modified)" } else if r["stock"] == false { "  (yours)" } else { "" };
            format!("{}\t{}{mark}", r["id"].as_str().unwrap_or(""), r["name"].as_str().unwrap_or(""))
        })
        .collect::<Vec<_>>()
        .join("\n");
    Answer::ok(json!({"layouts": rows}), text)
}

pub fn show(folder: &Folder, id: &str) -> Answer {
    match current(folder, id) {
        Some(v) => Answer::ok(json!({"layout": v, "stock": is_stock(id), "modified": is_stock(id) && user_text(folder, id).is_some()}),
            serde_json::to_string_pretty(&v).unwrap_or_default()),
        None => Answer::failed("unknown-layout", format!("there is no layout {id}")),
    }
}

/// Writes a layout file, with §7.7's rule and warning.
fn write(folder: &Folder, id: &str, value: &Value, modify_stock: bool, done: &str) -> Answer {
    if !is_layout_id(id) {
        return Answer::failed("bad-value", format!("{id:?} isn't a layout id (§7.1)"));
    }
    if is_stock(id) && !modify_stock {
        return Answer::failed("stock-layout", format!("{id} is a stock layout: {WHY_STOCK}"));
    }
    let text = serde_json::to_string_pretty(value).unwrap_or_default() + "\n";
    let outcome = folder.write(&format!("layouts/{id}.json"), &text);
    let mut a = if outcome.ok {
        Answer::ok(json!({"written": format!("layouts/{id}.json")}), done)
    } else {
        Answer { ok: false, value: json!({"written": null}), text: "Nothing was written.".into(), problems: vec![] }
    };
    a.problems = outcome.problems;
    if a.ok {
        // §7.7's two warnings. The engine says `no-model` for a new id itself; say why with it.
        if is_stock(id) {
            a.problems.push(Problem::warning("", "modified-stock", WHY_STOCK));
        } else if let Some(p) = a.problems.iter_mut().find(|p| p.code == "no-model") {
            p.message = WHY_NEW.to_string();
        } else {
            a.problems.push(Problem::warning("", "no-model", WHY_NEW));
        }
    }
    a
}

pub fn add(folder: &Folder, file: &str, modify_stock: bool) -> Answer {
    let text = match std::fs::read_to_string(file) {
        Ok(t) => t,
        Err(e) => return Answer::failed("not-read", format!("{file}: {e}")),
    };
    let Ok(value) = serde_json::from_str::<Value>(&text) else { return Answer::failed("bad-json", format!("{file} isn't JSON")) };
    let Some(id) = value.get("id").and_then(Value::as_str).map(String::from) else {
        return Answer::failed("missing-id", "a layout has an id");
    };
    write(folder, &id, &value, modify_stock, &format!("Added layout {id}."))
}

/// `layout set <id> <path> <json>`: one value of a layout (the user's file, or a copy of the stock one).
pub fn set(folder: &Folder, id: &str, at: &str, json_text: &str, modify_stock: bool) -> Answer {
    let Some(steps) = path::parse(at) else { return Answer::failed("bad-path", format!("{at:?} isn't a path")) };
    let Ok(new) = serde_json::from_str::<Value>(json_text) else { return Answer::failed("bad-json", format!("{json_text:?} isn't JSON")) };
    let Some(mut value) = current(folder, id) else { return Answer::failed("unknown-layout", format!("there is no layout {id}")) };
    if steps.first() == Some(&path::Step::Key("id".into())) {
        return Answer::failed("bad-value", "a layout's id doesn't change here: `bada layout copy` makes one under a new id");
    }
    if let Err(e) = path::set(&mut value, &steps, new) {
        return Answer::failed("bad-path", e);
    }
    write(folder, id, &value, modify_stock, &format!("Set {} of layout {id}.", path::shown(&steps)))
}

pub fn remove(folder: &Folder, id: &str) -> Answer {
    if is_stock(id) {
        return Answer::failed("stock-layout", format!("{id} is a stock layout and can't be removed: `bada layout reset {id}` undoes a change to it"));
    }
    if user_text(folder, id).is_none() {
        return Answer::failed("unknown-layout", format!("there is no layout {id} of yours"));
    }
    let outcome = folder.remove(&format!("layouts/{id}.json"));
    let mut a = if outcome.ok { Answer::ok(json!({"removed": id}), format!("Removed layout {id}.")) } else { Answer::failed("not-removed", "the file couldn't be removed") };
    a.problems.extend(outcome.problems);
    a
}

/// `layout reset <id>`: a stock layout as it ships, the user's change removed.
pub fn reset(folder: &Folder, id: &str) -> Answer {
    if !is_stock(id) {
        return Answer::failed("bad-value", format!("{id} isn't a stock layout: only a stock layout can be reset"));
    }
    if user_text(folder, id).is_none() {
        return Answer::ok(json!({"reset": id}), format!("{id} is already as it ships."));
    }
    let outcome = folder.remove(&format!("layouts/{id}.json"));
    let mut a = if outcome.ok { Answer::ok(json!({"reset": id}), format!("{id} is as it ships again.")) } else { Answer::failed("not-removed", "the file couldn't be removed") };
    a.problems.extend(outcome.problems);
    a
}

/// `layout copy <id> <new-id>`: a layout under a new id, to change without touching the stock one.
pub fn copy(folder: &Folder, id: &str, new_id: &str) -> Answer {
    let Some(mut value) = current(folder, id) else { return Answer::failed("unknown-layout", format!("there is no layout {id}")) };
    if current(folder, new_id).is_some() {
        return Answer::failed("duplicate-layout", format!("there is a layout {new_id} already"));
    }
    value["id"] = json!(new_id);
    write(folder, new_id, &value, false, &format!("Copied {id} to {new_id}."))
}

pub fn validate(folder: &Folder, file: &str) -> Answer {
    let text = match std::fs::read_to_string(file) {
        Ok(t) => t,
        Err(e) => return Answer::failed("not-read", format!("{file}: {e}")),
    };
    let (checked, id) = check::layout(folder, &text);
    let ok = checked.ok();
    let mut a = Answer::ok(json!({"id": id}), if ok { format!("{file}: no problems") } else { format!("{file}:") });
    a.problems = checked.problems;
    a.ok = ok;
    a
}

/// `layout missing <id>` (§7.4): the ASCII symbols and digits the layout can't type on a JIS
/// keyboard (with and without ⇧), and its free slots: ⇧ places that type nothing or repeat the
/// unshifted key, where such a character could go.
pub fn missing(folder: &Folder, id: &str) -> Answer {
    let Some(value) = current(folder, id) else { return Answer::failed("unknown-layout", format!("there is no layout {id}")) };
    let loaded = match Layout::load(&value, is_stock(id)) {
        Ok(l) => l,
        Err(problems) => return Answer::failed("bad-layout", format!("{id} doesn't load")).with_problems(problems),
    };
    let (missing, free) = reach(&loaded);
    let text = format!(
        "{id} can't type: {}\nfree slots: {}",
        if missing.is_empty() { "nothing".to_string() } else { missing.iter().collect::<String>() },
        if free.is_empty() { "none".to_string() } else { free.join(" ") }
    );
    Answer::ok(json!({"id": id, "missing": missing.iter().map(|c| c.to_string()).collect::<Vec<_>>(), "free": free}), text)
}

/// What a layout types on the 48 typing keys, without and with ⇧: the characters it can't reach
/// among `!`…`~` but the letters, and the ⇧ slots that are free.
pub fn reach(l: &Layout) -> (Vec<char>, Vec<String>) {
    let mut typed = std::collections::HashSet::new();
    let mut free = Vec::new();
    for &key in Key::ALL.iter().take(layout::KEYS).filter(|k| **k != Key::Space) {
        let base = l.slot(Press { key, layer: Layer::Base }).map(|s| s.text.clone());
        let shift = l.slot(Press { key, layer: Layer::Shift }).map(|s| s.text.clone());
        for t in base.iter().chain(shift.iter()) {
            typed.extend(t.chars());
        }
        if shift.is_none() || shift == base {
            free.push(format!("shift+{}", key.code()));
        }
    }
    let missing = (0x21u8..0x7F).map(char::from).filter(|c| !c.is_ascii_alphabetic() && !typed.contains(c)).collect();
    (missing, free)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn stock(id: &str) -> Layout {
        Layout::load(&serde_json::from_str(stock_text(id).unwrap()).unwrap(), true).unwrap()
    }

    #[test]
    fn the_standards_table_of_what_stock_layouts_cant_type() {
        // §7.4: en-qwerty types every ASCII character; ⇧0 is its one free slot.
        let (missing, free) = reach(&stock("en-qwerty"));
        assert!(missing.is_empty(), "{missing:?}");
        assert_eq!(free, vec!["shift+Digit0"]);
        for id in ["en-qwerty-us", "en-dvorak", "en-colemak", "ko-2-set"] {
            assert!(reach(&stock(id)).0.is_empty(), "{id}");
        }
    }
}
