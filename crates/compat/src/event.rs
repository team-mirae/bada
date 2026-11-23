//! Events (the standard, §5): what a harness passes to `P_send`, and how the JSON is read.

use serde::Deserialize;
use serde_json::Value;

use crate::reply::{Error, code};
use crate::{KeyEvent, Mode};

/// Why what is staged is committed.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Cause {
    /// The field lost focus.
    Blur,
    /// A click in the text.
    Click,
    /// The app changed the text.
    Edit,
    #[default]
    Harness,
}

/// The harness's answer to a reply's `ask`.
#[derive(Clone, Debug, Default, PartialEq, Eq, Deserialize)]
#[serde(default)]
pub struct Text {
    /// The selected text; `""` if nothing is selected or something is marked.
    pub selection: Option<String>,
    /// The text before and after the caret, no longer than asked.
    pub before: Option<String>,
    pub after: Option<String>,
}

/// An event about one text field.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FieldEvent {
    Key(KeyEvent),
    Action { name: String },
    Select { mode: Mode, profile: Option<String> },
    Pick { row: usize },
    Text(Text),
    Flush { cause: Cause },
    Reset,
    State,
}

/// What a `settings`, `layout` or `dictionary` event asks.
#[derive(Clone, Debug, PartialEq)]
pub enum Change {
    /// Nothing to change: re-read what is in effect, and the problems of the last change.
    Read,
    /// Replace with this (`settings` with `set`; `layout` or `dictionary` with a `value`).
    Set(Value),
    /// `settings` with `patch`: a JSON merge patch (RFC 7396).
    Patch(Value),
    /// `layout` with an `id` and `value: null`, or `dictionary` with `value: null`.
    Remove(Option<String>),
}

/// An event of the session: it needs no engine.
#[derive(Clone, Debug, PartialEq)]
pub enum SessionEvent {
    Settings { change: Change, dry: bool },
    Layout { change: Change, dry: bool },
    Dictionary { change: Change },
}

#[derive(Clone, Debug, PartialEq)]
pub enum Event {
    Field(FieldEvent),
    Session(SessionEvent),
}

impl Event {
    /// The event's `type`.
    pub fn kind(&self) -> &'static str {
        match self {
            Event::Field(FieldEvent::Key(_)) => "key",
            Event::Field(FieldEvent::Action { .. }) => "action",
            Event::Field(FieldEvent::Select { .. }) => "select",
            Event::Field(FieldEvent::Pick { .. }) => "pick",
            Event::Field(FieldEvent::Text(_)) => "text",
            Event::Field(FieldEvent::Flush { .. }) => "flush",
            Event::Field(FieldEvent::Reset) => "reset",
            Event::Field(FieldEvent::State) => "state",
            Event::Session(SessionEvent::Settings { .. }) => "settings",
            Event::Session(SessionEvent::Layout { .. }) => "layout",
            Event::Session(SessionEvent::Dictionary { .. }) => "dictionary",
        }
    }
}

/// The event types the standard has, in its order.
pub const TYPES: [&str; 11] =
    ["key", "action", "select", "pick", "text", "flush", "reset", "state", "settings", "layout", "dictionary"];
/// The ones every engine takes.
pub const REQUIRED: [&str; 5] = ["key", "select", "flush", "reset", "state"];

fn bad(value: Value, what: &str) -> Error {
    let _ = value;
    Error::new(code::BAD_EVENT, what)
}

/// A typed part of the event, or `bad-event` with the field at fault where serde names it.
fn part<T: for<'de> Deserialize<'de>>(value: &Value) -> Result<T, Error> {
    T::deserialize(value).map_err(|e| {
        let message = e.to_string();
        let path = message.split('`').nth(1).filter(|_| message.starts_with("missing field")).map(str::to_string);
        Error { code: code::BAD_EVENT.into(), message, path }
    })
}

fn flag(value: &Value, name: &str) -> Result<bool, Error> {
    match value.get(name) {
        None => Ok(false),
        Some(Value::Bool(b)) => Ok(*b),
        Some(_) => Err(Error::at(code::BAD_EVENT, name, format!("{name} is true or false"))),
    }
}

fn object(value: &Value, name: &str) -> Result<Option<Value>, Error> {
    match value.get(name) {
        None => Ok(None),
        Some(v @ Value::Object(_)) => Ok(Some(v.clone())),
        Some(_) => Err(Error::at(code::BAD_EVENT, name, format!("{name} is an object"))),
    }
}

/// Reads one event. `None`: a NULL pointer, or text that isn't UTF-8.
///
/// Fields the standard doesn't have are ignored, in an event whose type it has.
pub fn parse(text: Option<&str>) -> Result<Event, Error> {
    let not_json = || Error::new(code::BAD_JSON, "the event isn't a JSON object");
    let value: Value = serde_json::from_str(text.ok_or_else(not_json)?).map_err(|_| not_json())?;
    if !value.is_object() {
        return Err(not_json());
    }
    let Some(kind) = value.get("type").and_then(Value::as_str) else {
        return Err(Error::at(code::BAD_EVENT, "type", "an event has a type"));
    };
    let field = |e| Ok(Event::Field(e));
    match kind {
        "key" => field(FieldEvent::Key(part(&value)?)),
        "action" => {
            #[derive(Deserialize)]
            struct Action {
                name: String,
            }
            field(FieldEvent::Action { name: part::<Action>(&value)?.name })
        }
        "select" => {
            #[derive(Deserialize)]
            struct Select {
                mode: Mode,
                #[serde(default)]
                profile: Option<String>,
            }
            let s: Select = part(&value)?;
            if s.mode == Mode::Henkan && s.profile.is_none() {
                return Err(Error::at(code::BAD_EVENT, "profile", "select with henkan names a profile"));
            }
            field(FieldEvent::Select { mode: s.mode, profile: s.profile })
        }
        "pick" => {
            #[derive(Deserialize)]
            struct Pick {
                row: usize,
            }
            field(FieldEvent::Pick { row: part::<Pick>(&value)?.row })
        }
        "text" => field(FieldEvent::Text(part(&value)?)),
        "flush" => {
            #[derive(Deserialize)]
            struct Flush {
                #[serde(default)]
                cause: Cause,
            }
            field(FieldEvent::Flush { cause: part::<Flush>(&value)?.cause })
        }
        "reset" => field(FieldEvent::Reset),
        "state" => field(FieldEvent::State),
        "settings" => {
            let (set, patch) = (object(&value, "set")?, object(&value, "patch")?);
            let change = match (set, patch) {
                (Some(_), Some(_)) => return Err(bad(value, "settings has set or patch, not both")),
                (Some(v), None) => Change::Set(v),
                (None, Some(v)) => Change::Patch(v),
                (None, None) => Change::Read,
            };
            Ok(Event::Session(SessionEvent::Settings { change, dry: flag(&value, "dry")? }))
        }
        "layout" => {
            let id = match value.get("id") {
                None => None,
                Some(Value::String(s)) => Some(s.clone()),
                Some(_) => return Err(Error::at(code::BAD_EVENT, "id", "id is a string")),
            };
            let change = match value.get("value") {
                None if id.is_some() => {
                    return Err(Error::at(code::BAD_EVENT, "value", "an id comes with value: null"));
                }
                None => Change::Read,
                Some(Value::Null) if id.is_none() => {
                    return Err(Error::at(code::BAD_EVENT, "id", "removing a layout needs its id"));
                }
                Some(Value::Null) => Change::Remove(id),
                Some(v @ Value::Object(_)) => Change::Set(v.clone()),
                Some(_) => return Err(Error::at(code::BAD_EVENT, "value", "value is a layout object, or null")),
            };
            Ok(Event::Session(SessionEvent::Layout { change, dry: flag(&value, "dry")? }))
        }
        "dictionary" => {
            let change = match value.get("value") {
                None => Change::Read,
                Some(Value::Null) => Change::Remove(None),
                Some(v @ Value::Object(_)) => Change::Set(v.clone()),
                Some(_) => return Err(Error::at(code::BAD_EVENT, "value", "value is a dictionary object, or null")),
            };
            Ok(Event::Session(SessionEvent::Dictionary { change }))
        }
        other => Err(Error::new(code::UNKNOWN_EVENT, format!("no event type named {other}"))),
    }
}

/// Applies a JSON merge patch (RFC 7396): an object merges key by key, null removes a key, and
/// anything else replaces.
pub fn merge_patch(target: &mut Value, patch: &Value) {
    let Value::Object(patch) = patch else {
        *target = patch.clone();
        return;
    };
    if !target.is_object() {
        *target = Value::Object(Default::default());
    }
    let map = target.as_object_mut().expect("an object");
    for (key, value) in patch {
        if value.is_null() {
            map.remove(key);
        } else {
            merge_patch(map.entry(key.clone()).or_insert(Value::Null), value);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn kind(text: &str) -> Result<&'static str, String> {
        parse(Some(text)).map(|e| e.kind()).map_err(|e| e.code)
    }

    #[test]
    fn every_type_reads() {
        assert_eq!(kind(r#"{"type":"key","key":"KeyA","shift":true,"repeat":true}"#), Ok("key"));
        assert_eq!(kind(r#"{"type":"action","name":"menu.next"}"#), Ok("action"));
        assert_eq!(kind(r#"{"type":"select","mode":"henkan","profile":"main"}"#), Ok("select"));
        assert_eq!(kind(r#"{"type":"select","mode":"muhenkan","profile":null}"#), Ok("select"));
        assert_eq!(kind(r#"{"type":"pick","row":2}"#), Ok("pick"));
        assert_eq!(kind(r#"{"type":"text","selection":"한"}"#), Ok("text"));
        assert_eq!(kind(r#"{"type":"flush","cause":"blur"}"#), Ok("flush"));
        assert_eq!(kind(r#"{"type":"flush"}"#), Ok("flush"));
        assert_eq!(kind(r#"{"type":"reset"}"#), Ok("reset"));
        assert_eq!(kind(r#"{"type":"state","extra":1}"#), Ok("state"));
        assert_eq!(kind(r#"{"type":"settings","set":{}}"#), Ok("settings"));
        assert_eq!(kind(r#"{"type":"settings"}"#), Ok("settings"));
        assert_eq!(kind(r#"{"type":"layout","value":{"id":"x"}}"#), Ok("layout"));
        assert_eq!(kind(r#"{"type":"layout","id":"en-x","value":null}"#), Ok("layout"));
        assert_eq!(kind(r#"{"type":"dictionary","value":null}"#), Ok("dictionary"));
    }

    #[test]
    fn what_is_wrong_has_its_code() {
        for text in ["", "{", "null", "[]", "7"] {
            assert_eq!(kind(text), Err("bad-json".into()), "{text:?}");
        }
        assert_eq!(parse(None).unwrap_err().code, "bad-json");
        for text in [
            r#"{}"#,
            r#"{"type":7}"#,
            r#"{"type":"key"}"#,
            r#"{"type":"key","key":7}"#,
            r#"{"type":"select","mode":"henkan"}"#,
            r#"{"type":"select","mode":"other"}"#,
            r#"{"type":"pick","row":-1}"#,
            r#"{"type":"flush","cause":"because"}"#,
            r#"{"type":"settings","set":{},"patch":{}}"#,
            r#"{"type":"settings","set":7}"#,
            r#"{"type":"layout","value":null}"#,
            r#"{"type":"layout","id":"en-x"}"#,
            r#"{"type":"dictionary","value":7}"#,
        ] {
            assert_eq!(kind(text), Err("bad-event".into()), "{text}");
        }
        assert_eq!(kind(r#"{"type":"focus"}"#), Err("unknown-event".into()));
        assert_eq!(parse(Some(r#"{"type":"key"}"#)).unwrap_err().path.as_deref(), Some("key"));
    }

    #[test]
    fn merge_patch_is_rfc_7396() {
        let mut v = json!({"a": {"b": 1, "c": 2}, "d": [1, 2]});
        merge_patch(&mut v, &json!({"a": {"b": null, "e": 3}, "d": [9]}));
        assert_eq!(v, json!({"a": {"c": 2, "e": 3}, "d": [9]}));
    }
}
