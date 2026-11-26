//! The `keys` part of a settings file, checked (the standard, §9, §10.1). The command line has no
//! key table to carry out: it only says what is wrong with one, as every harness does when it reads
//! the file. An error means the file can't be used; a warning is taken, and that action or binding
//! never applies.

use bada_compat::{LayoutInfo, Problem};
use serde_json::Value;

/// The language keys that can make a pair (§9.3): every one but Caps Lock.
const LANGUAGE_KEYS: [&str; 6] = ["Backquote", "NonConvert", "Convert", "KanaMode", "Lang2", "Lang1"];
const MODS: [&str; 4] = ["ctrl", "alt", "shift", "meta"];
const FIELDS: [&str; 5] = ["bindings", "cycle", "henkan_enters", "caps_lock", "caps_rule"];

/// `[a-z0-9-]{1,32}` (§8.1).
pub fn is_profile_id(text: &str) -> bool {
    (1..=32).contains(&text.len()) && text.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
}

/// Lowercase letters, digits and hyphens, with no empty part.
fn is_slug(text: &str) -> bool {
    !text.is_empty()
        && text.split('-').all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit()))
}

/// `<group>-<name>` (§7.1): a group of two or three letters, then at least one more part.
pub fn is_layout_id(text: &str) -> bool {
    let mut parts = text.split('-');
    let group = parts.next().unwrap_or("");
    text.len() <= 48
        && is_slug(text)
        && parts.next().is_some()
        && (2..=3).contains(&group.len())
        && group.bytes().all(|b| b.is_ascii_lowercase())
}

/// A W3C code: a capital, then letters and digits.
fn is_code(text: &str) -> bool {
    text.as_bytes().first().is_some_and(u8::is_ascii_uppercase) && text.bytes().all(|b| b.is_ascii_alphanumeric())
}

/// What a binding is on.
#[derive(Debug, PartialEq, Eq)]
pub enum On {
    /// One key, with its modifiers in the order ctrl, alt, shift, meta.
    Key { code: String, mods: [bool; 4] },
    /// Two language keys held together.
    Pair(String, String),
}

impl On {
    /// Reads `on`. The modifiers are taken in any order here: [`On::name`] says how they are written.
    pub fn parse(text: &str) -> Option<On> {
        let mut parts: Vec<&str> = text.split('+').collect();
        let code = parts.pop()?;
        if !is_code(code) {
            return None;
        }
        if parts.len() == 1 && is_code(parts[0]) {
            let pair = LANGUAGE_KEYS.contains(&parts[0]) && LANGUAGE_KEYS.contains(&code) && parts[0] != code;
            return pair.then(|| On::Pair(parts[0].to_string(), code.to_string()));
        }
        let mut mods = [false; 4];
        for part in parts {
            let at = MODS.iter().position(|m| *m == part)?;
            if mods[at] {
                return None;
            }
            mods[at] = true;
        }
        Some(On::Key { code: code.to_string(), mods })
    }

    /// As `on` writes it.
    pub fn name(&self) -> String {
        match self {
            On::Key { code, mods } => {
                let mut parts: Vec<&str> = MODS.iter().zip(mods).filter(|(_, on)| **on).map(|(m, _)| *m).collect();
                parts.push(code);
                parts.join("+")
            }
            On::Pair(a, b) => {
                let (a, b) = if a <= b { (a, b) } else { (b, a) };
                format!("{a}+{b}")
            }
        }
    }
}

/// `<layout id>.<action id>`, if the action is written as one.
fn layout_action(action: &str) -> Option<(&str, &str)> {
    let (layout, id) = action.split_once('.')?;
    (is_layout_id(layout) && is_slug(id)).then_some((layout, id))
}

/// Whether an action is one the table's format has (§9.2).
pub fn is_action(action: &str) -> bool {
    const FIXED: [&str; 14] = [
        "mode.toggle",
        "mode:muhenkan",
        "mode:henkan",
        "profile.next",
        "profile.prev",
        "profile.last",
        "profile.first",
        "menu.next",
        "menu.prev",
        "fix",
        "width.full",
        "width.half",
        "pass",
        "none",
    ];
    FIXED.contains(&action)
        || action.strip_prefix("profile:").is_some_and(is_profile_id)
        || layout_action(action).is_some()
}

/// `muhenkan`, `henkan` or `profile:<id>`.
fn is_when(when: &str) -> bool {
    when == "muhenkan" || when == "henkan" || when.strip_prefix("profile:").is_some_and(is_profile_id)
}

/// What is wrong with `keys`. `profiles`: the ids of the file's own henkan profiles. `layouts`: the
/// session's layouts, for a layout's action. Paths are into the whole file (`keys.bindings[0].on`).
pub fn check(keys: Option<&Value>, profiles: &[String], layouts: &[LayoutInfo]) -> Vec<Problem> {
    let mut problems = Vec::new();
    let Some(keys) = keys else { return problems };
    let Some(keys) = keys.as_object() else {
        problems.push(Problem::error("keys", "bad-value", "keys is an object"));
        return problems;
    };
    let has = |id: &str| profiles.iter().any(|p| p == id);
    for name in keys.keys().filter(|name| !FIELDS.contains(&name.as_str())) {
        problems.push(Problem::error(&format!("keys.{name}"), "unknown-field", format!("keys has no field {name}")));
    }

    let bindings = keys.get("bindings");
    if bindings.is_some_and(|b| !b.is_array()) {
        problems.push(Problem::error("keys.bindings", "bad-value", "bindings is a list"));
    }
    for (i, binding) in bindings.and_then(Value::as_array).into_iter().flatten().enumerate() {
        let at = format!("keys.bindings[{i}]");
        let Some(binding) = binding.as_object() else {
            problems.push(Problem::error(&at, "bad-value", "a binding is an object"));
            continue;
        };
        for name in binding.keys().filter(|name| !["on", "do", "when"].contains(&name.as_str())) {
            let message = format!("a binding has no field {name}");
            problems.push(Problem::error(&format!("{at}.{name}"), "unknown-field", message));
        }
        let on = binding.get("on").and_then(Value::as_str);
        match on.and_then(On::parse) {
            None => problems.push(Problem::error(
                &format!("{at}.on"),
                "bad-value",
                "on is one key with ctrl, alt, shift and meta before it, or two different language keys",
            )),
            Some(key @ On::Key { .. }) if on != Some(key.name().as_str()) => problems.push(Problem::error(
                &format!("{at}.on"),
                "bad-value",
                format!("the modifiers are written in the order ctrl, alt, shift, meta: {}", key.name()),
            )),
            Some(_) => {}
        }
        let actions = binding.get("do").and_then(Value::as_array);
        if actions.is_none_or(|list| list.is_empty()) {
            problems.push(Problem::error(&format!("{at}.do"), "bad-value", "do is a list of at least one action"));
        }
        for (j, action) in actions.into_iter().flatten().enumerate() {
            let path = format!("{at}.do[{j}]");
            let Some(action) = action.as_str().filter(|a| is_action(a)) else {
                problems.push(Problem::error(&path, "unknown-action", format!("no action is written {action}")));
                continue;
            };
            if let Some(id) = action.strip_prefix("profile:").filter(|id| !has(id)) {
                let message = format!("no profile named {id}: this action never applies");
                problems.push(Problem::warning(&path, "unknown-profile", message));
            }
            if let Some((layout, id)) = layout_action(action) {
                match layouts.iter().find(|l| l.id == layout) {
                    None => {
                        problems.push(Problem::warning(&path, "unknown-layout", format!("no layout named {layout}")));
                    }
                    Some(l) if !l.actions.iter().any(|a| a.id == id) => {
                        problems.push(Problem::warning(
                            &path,
                            "unknown-action",
                            format!("{layout} has no action {id}"),
                        ));
                    }
                    Some(_) => {}
                }
            }
        }
        if let Some(when) = binding.get("when") {
            let path = format!("{at}.when");
            match when.as_str().filter(|w| is_when(w)) {
                None => {
                    problems.push(Problem::error(&path, "bad-value", "when is muhenkan, henkan or profile:<id>"));
                }
                Some(when) => {
                    if let Some(id) = when.strip_prefix("profile:").filter(|id| !has(id)) {
                        let message = format!("no profile named {id}: this binding never holds");
                        problems.push(Problem::warning(&path, "unknown-profile", message));
                    }
                }
            }
        }
    }

    if let Some(cycle) = keys.get("cycle") {
        let ids: Option<Vec<&str>> = cycle
            .as_array()
            .and_then(|list| list.iter().map(|id| id.as_str().filter(|id| is_profile_id(id))).collect());
        match ids {
            None => problems.push(Problem::error("keys.cycle", "bad-value", "cycle is a list of profile ids")),
            Some(ids) => {
                for (i, id) in ids.iter().enumerate() {
                    let path = format!("keys.cycle[{i}]");
                    if ids[..i].contains(id) {
                        problems.push(Problem::error(&path, "bad-value", format!("{id} is listed twice")));
                    } else if !has(id) {
                        let message = format!("no profile named {id}: the cycle skips it");
                        problems.push(Problem::warning(&path, "unknown-profile", message));
                    }
                }
            }
        }
    }
    if let Some(enters) = keys.get("henkan_enters") {
        match enters.as_str().filter(|id| is_profile_id(id)) {
            None => problems.push(Problem::error(
                "keys.henkan_enters",
                "bad-value",
                "henkan_enters is last or a profile id",
            )),
            Some(id) if id != "last" && !has(id) => problems.push(Problem::warning(
                "keys.henkan_enters",
                "unknown-profile",
                format!("no profile named {id}: henkan enters the first profile"),
            )),
            Some(_) => {}
        }
    }
    for (name, values) in [("caps_lock", ["transient", "respect"]), ("caps_rule", ["allcaps", "upper_snake"])] {
        if keys.get(name).is_some_and(|v| !v.as_str().is_some_and(|v| values.contains(&v))) {
            let message = format!("{name} is {} or {}", values[0], values[1]);
            problems.push(Problem::error(&format!("keys.{name}"), "bad-value", message));
        }
    }
    problems
}

#[cfg(test)]
mod tests {
    use super::*;
    use bada_compat::Level;
    use serde_json::json;

    fn layouts() -> Vec<LayoutInfo> {
        let describe: Value = serde_json::from_str(crate::check::describe()).unwrap();
        serde_json::from_value(describe["layouts"].clone()).unwrap()
    }

    fn said(keys: Value, profiles: &[&str]) -> Vec<String> {
        let profiles: Vec<String> = profiles.iter().map(|p| p.to_string()).collect();
        check(Some(&keys), &profiles, &layouts())
            .iter()
            .map(|p| format!("{} {} {}", if p.level == Level::Error { "error" } else { "warning" }, p.path, p.code))
            .collect()
    }

    #[test]
    fn chords_and_pairs() {
        assert_eq!(On::parse("Lang1").unwrap().name(), "Lang1");
        assert_eq!(On::parse("ctrl+alt+shift+meta+Space").unwrap().name(), "ctrl+alt+shift+meta+Space");
        assert_eq!(On::parse("shift+ctrl+Lang1").unwrap().name(), "ctrl+shift+Lang1", "read in any order");
        assert_eq!(On::parse("Lang1+KanaMode").unwrap().name(), "KanaMode+Lang1");
        for bad in ["", "lang1", "shift+shift+Tab", "hyper+Tab", "Lang1+Lang1", "CapsLock+Lang1", "KeyA+KeyB", "shift+"]
        {
            assert_eq!(On::parse(bad), None, "{bad}");
        }
    }

    #[test]
    fn actions_the_format_has() {
        for good in
            ["mode.toggle", "mode:henkan", "profile:ko", "profile.first", "menu.prev", "ja-kana.katakana", "pass"]
        {
            assert!(is_action(good), "{good}");
        }
        for bad in ["jump", "mode:other", "profile:", "profile:Main", "kana.katakana", "ja-kana.", "ja-kana.Kata", ""] {
            assert!(!is_action(bad), "{bad}");
        }
        assert!(is_layout_id("ko-2-set") && is_layout_id("uni-symbols"));
        assert!(
            !is_layout_id("ko") && !is_layout_id("korean-2-set") && !is_layout_id("ko--set") && !is_layout_id("Ko-set")
        );
    }

    #[test]
    fn a_good_table_has_no_problems() {
        assert_eq!(check(None, &[], &[]), vec![]);
        let keys = json!({
            "bindings": [
                {"on": "Lang2", "do": ["mode:muhenkan"]},
                {"on": "Lang1", "do": ["menu.next", "profile:ko"], "when": "henkan"},
                {"on": "shift+Tab", "do": ["menu.prev", "mode.toggle"]},
                {"on": "Lang1+KanaMode", "do": ["profile.last"], "when": "profile:ko"},
                {"on": "F13", "do": ["ja-kana.katakana", "pass"]}
            ],
            "cycle": ["ko"], "henkan_enters": "last", "caps_lock": "transient", "caps_rule": "upper_snake"
        });
        assert_eq!(said(keys, &["ko"]), Vec::<String>::new());
        assert_eq!(said(json!({"bindings": []}), &[]), Vec::<String>::new(), "no bindings at all is a table too");
    }

    #[test]
    fn each_flaw_is_an_error_with_its_path() {
        assert_eq!(said(json!([]), &[]), ["error keys bad-value"]);
        assert_eq!(said(json!({"extra": 1}), &[]), ["error keys.extra unknown-field"]);
        assert_eq!(said(json!({"bindings": {}}), &[]), ["error keys.bindings bad-value"]);
        assert_eq!(said(json!({"bindings": [1]}), &[]), ["error keys.bindings[0] bad-value"]);
        assert_eq!(
            said(json!({"bindings": [{"on": "shift+ctrl+Lang1", "do": ["fix"], "why": 1}]}), &[]),
            ["error keys.bindings[0].why unknown-field", "error keys.bindings[0].on bad-value"]
        );
        assert_eq!(
            said(json!({"bindings": [{"on": "Lang1+Lang1", "do": ["fix"]}]}), &[]),
            ["error keys.bindings[0].on bad-value"]
        );
        assert_eq!(said(json!({"bindings": [{"do": ["fix"]}]}), &[]), ["error keys.bindings[0].on bad-value"]);
        assert_eq!(
            said(json!({"bindings": [{"on": "Lang1", "do": []}]}), &[]),
            ["error keys.bindings[0].do bad-value"]
        );
        assert_eq!(said(json!({"bindings": [{"on": "Lang1"}]}), &[]), ["error keys.bindings[0].do bad-value"]);
        assert_eq!(
            said(json!({"bindings": [{"on": "Lang1", "do": ["jump", 3]}]}), &[]),
            ["error keys.bindings[0].do[0] unknown-action", "error keys.bindings[0].do[1] unknown-action"]
        );
        assert_eq!(
            said(json!({"bindings": [{"on": "Lang1", "do": ["fix"], "when": "sometimes"}]}), &[]),
            ["error keys.bindings[0].when bad-value"]
        );
        assert_eq!(said(json!({"cycle": "ko"}), &["ko"]), ["error keys.cycle bad-value"]);
        assert_eq!(said(json!({"cycle": ["ko", "Ko"]}), &["ko"]), ["error keys.cycle bad-value"]);
        assert_eq!(said(json!({"cycle": ["ko", "ko"]}), &["ko"]), ["error keys.cycle[1] bad-value"]);
        assert_eq!(said(json!({"henkan_enters": 1}), &[]), ["error keys.henkan_enters bad-value"]);
        assert_eq!(said(json!({"caps_lock": "toggle"}), &[]), ["error keys.caps_lock bad-value"]);
        assert_eq!(said(json!({"caps_rule": true}), &[]), ["error keys.caps_rule bad-value"]);
    }

    #[test]
    fn what_names_something_that_isnt_there_is_a_warning() {
        let keys = json!({
            "bindings": [
                {"on": "Convert", "do": ["profile:gone", "menu.next"], "when": "profile:gone"},
                {"on": "F13", "do": ["ja-nothing.katakana", "ja-kana.nothing", "ja-kana.katakana"]}
            ],
            "cycle": ["ko", "gone"], "henkan_enters": "gone"
        });
        assert_eq!(
            said(keys, &["ko"]),
            [
                "warning keys.bindings[0].do[0] unknown-profile",
                "warning keys.bindings[0].when unknown-profile",
                "warning keys.bindings[1].do[0] unknown-layout",
                "warning keys.bindings[1].do[1] unknown-action",
                "warning keys.cycle[1] unknown-profile",
                "warning keys.henkan_enters unknown-profile",
            ]
        );
    }

    #[test]
    fn the_standards_own_tables_are_good() {
        let root = concat!(env!("CARGO_MANIFEST_DIR"), "/..");
        let cases: Value =
            serde_json::from_str(&std::fs::read_to_string(format!("{root}/docs/cases/key-table.json")).unwrap())
                .unwrap();
        let four: Vec<String> = cases["profiles"]["four"]
            .as_array()
            .unwrap()
            .iter()
            .map(|p| p["id"].as_str().unwrap().to_string())
            .collect();
        for (name, table) in cases["tables"].as_object().unwrap() {
            let problems = check(Some(table), &four, &layouts());
            let errors: Vec<&Problem> = problems.iter().filter(|p| p.level == Level::Error).collect();
            assert!(errors.is_empty(), "{name}: {errors:?}");
            // `odd` names a profile that isn't there, on purpose; the others name only what is.
            assert_eq!(problems.is_empty(), name != "odd", "{name}: {problems:?}");
        }
        let example: Value = serde_json::from_str(
            &std::fs::read_to_string(format!("{root}/docs/schemas/examples/settings.json")).unwrap(),
        )
        .unwrap();
        let profiles: Vec<String> = example["engine"]["henkan"]["profiles"]
            .as_array()
            .unwrap()
            .iter()
            .map(|p| p["id"].as_str().unwrap().to_string())
            .collect();
        assert_eq!(check(example.get("keys"), &profiles, &layouts()), vec![]);
    }
}
