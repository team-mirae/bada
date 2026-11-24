//! The engine's part of the conformance checklist (docs/bada-compatible-v2.md §13), through the C
//! ABI that `export!` generates, on the example engine `shout`.

use std::ffi::{CStr, CString, c_char};
use std::thread;

use bada_compat::{Reply, SessionReply};
use serde_json::{Value, json};

#[allow(dead_code)]
#[path = "../examples/shout.rs"]
mod shout;
use shout::ffi as abi;

const HARNESS: &str = r#"{"id": "com.teammirae.bada.test", "version": "0.1.0", "features": ["segments", "pick"]}"#;
const PROFILES: &str =
    r#"{"henkan": {"profiles": [{"id": "main", "name": "Main"}, {"id": "other", "name": "Other"}]}}"#;

fn s(text: &str) -> CString {
    CString::new(text).unwrap()
}

fn read(ptr: *const c_char) -> Value {
    assert!(!ptr.is_null(), "a reply is never NULL for a session that isn't");
    let text = unsafe { CStr::from_ptr(ptr) }.to_str().expect("UTF-8");
    serde_json::from_str(text).unwrap_or_else(|e| panic!("not JSON ({e}): {text}"))
}

struct Session(*mut abi::Session);
// One session, used from several threads: it locks itself (§3.4).
unsafe impl Send for Session {}
unsafe impl Sync for Session {}

impl Session {
    /// A session whose settings have two profiles.
    fn new() -> Session {
        Session::with(&format!(r#"{{"harness": {HARNESS}, "settings": {PROFILES}}}"#))
    }

    fn with(init: &str) -> Session {
        let session = abi::session_new(std::ptr::null(), s(init).as_ptr());
        assert!(!session.is_null(), "P_session_new");
        Session(session)
    }

    /// An event with no engine.
    fn send(&self, event: &str) -> Value {
        read(abi::send(self.0, std::ptr::null_mut(), s(event).as_ptr()))
    }

    fn settings(&self, event: Value) -> SessionReply {
        serde_json::from_value(self.send(&event.to_string())).expect("a session reply")
    }

    /// A text field in henkan, profile `main`.
    fn field(&self) -> Field<'_> {
        let field = self.raw();
        assert!(field.send(r#"{"type":"select","mode":"henkan","profile":"main"}"#)["ok"] == true);
        field
    }

    /// A text field that has had no `select`.
    fn raw(&self) -> Field<'_> {
        let engine = abi::new(self.0);
        assert!(!engine.is_null(), "P_new");
        Field { session: self, engine }
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        abi::session_free(self.0);
    }
}

struct Field<'a> {
    session: &'a Session,
    engine: *mut abi::Engine,
}

impl Field<'_> {
    fn send(&self, event: &str) -> Value {
        read(abi::send(self.session.0, self.engine, s(event).as_ptr()))
    }

    fn reply(&self, event: &str) -> Reply {
        let value = self.send(event);
        assert_eq!(value["ok"], true, "{event}: {value}");
        serde_json::from_value(value).expect("a reply")
    }

    fn key(&self, code: &str) -> Reply {
        self.reply(&json!({"type": "key", "key": code}).to_string())
    }

    fn error(&self, event: &str) -> String {
        let value = self.send(event);
        assert_eq!(value["ok"], false, "{event}: {value}");
        assert_eq!(value.as_object().unwrap().len(), 2, "a refusal is ok and error: {value}");
        value["error"]["code"].as_str().expect("a code").to_string()
    }

    fn state(&self) -> Reply {
        self.reply(r#"{"type":"state"}"#)
    }

    /// Stages two letters.
    fn stage(&self) -> Reply {
        self.key("KeyA");
        let reply = self.key("KeyB");
        assert_eq!(reply.preedit.text, "AB");
        reply
    }
}

impl Drop for Field<'_> {
    fn drop(&mut self) {
        abi::free(self.engine);
    }
}

#[test]
fn describe_answers_before_anything_is_loaded() {
    let d = read(abi::describe());
    assert_eq!(d["abi"], json!({"name": "bada-compatible", "major": 2, "minor": 0}));
    let events: Vec<&str> = d["events"].as_array().unwrap().iter().filter_map(Value::as_str).collect();
    for required in bada_compat::REQUIRED {
        assert!(events.contains(&required), "{required}");
    }
    assert_eq!(d["engine"]["version"], env!("CARGO_PKG_VERSION"));
    assert_eq!(d["settings"]["format"], "bada-settings/2");
    assert!(d["limits"]["profiles"].as_u64().unwrap() >= 32);
    assert_eq!(abi::describe(), abi::describe(), "the same string every time");
}

#[test]
fn unknown_keys_pass_on() {
    let session = Session::new();
    let field = session.field();
    for code in ["NoSuchKey", "", "Other", "F13", "MediaPlayPause", "CapsLock"] {
        assert!(!field.key(code).consumed, "{code:?}");
    }
}

#[test]
fn bad_events_are_refused_and_change_nothing() {
    let session = Session::new();
    let field = session.field();
    let before = field.stage();
    for (event, code) in [
        ("", "bad-json"),
        ("{", "bad-json"),
        ("null", "bad-json"),
        (r#"{"shift":true}"#, "bad-event"),
        (r#"{"type":"key"}"#, "bad-event"),
        (r#"{"type":"key","key":7}"#, "bad-event"),
        (r#"{"type":"select","mode":"henkan"}"#, "bad-event"),
        (r#"{"type":"focus"}"#, "unknown-event"),
        (r#"{"type":"layout"}"#, "unknown-event"),
        (r#"{"type":"dictionary","value":null}"#, "unknown-event"),
        (r#"{"type":"action","name":"no.such"}"#, "unknown-action"),
        (r#"{"type":"pick","row":9}"#, "bad-row"),
        (r#"{"type":"select","mode":"henkan","profile":"nobody"}"#, "unknown-profile"),
    ] {
        assert_eq!(field.error(event), code, "{event}");
        let after = field.state();
        assert_eq!(
            (&after.preedit, &after.menu, &after.staged),
            (&before.preedit, &before.menu, &before.staged),
            "{event}"
        );
    }
    assert_eq!(read(abi::send(session.0, field.engine, std::ptr::null()))["error"]["code"], "bad-json");
    // An event about a text field, with no engine.
    assert_eq!(session.send(r#"{"type":"key","key":"KeyA"}"#)["error"]["code"], "needs-engine");
    assert_eq!(session.send(r#"{"type":"flush"}"#)["error"]["code"], "needs-engine");
}

#[test]
fn select_commits_when_something_changes_and_only_then() {
    let session = Session::new();
    let field = session.field();
    field.stage();
    let same = field.reply(r#"{"type":"select","mode":"henkan","profile":"main"}"#);
    assert_eq!((same.commit.as_str(), same.preedit.text.as_str()), ("", "AB"), "neither changed: nothing happens");
    let other = field.reply(r#"{"type":"select","mode":"henkan","profile":"other"}"#);
    assert_eq!(
        (other.commit.as_str(), other.preedit.text.as_str(), other.profile.as_deref()),
        ("AB", "", Some("other"))
    );
    field.stage();
    let out = field.reply(r#"{"type":"select","mode":"muhenkan","profile":"other"}"#);
    assert_eq!((out.commit.as_str(), out.preedit.text.as_str()), ("AB", ""));
    assert_eq!(out.mode, Some(bada_compat::Mode::Muhenkan));
    assert!(!field.key("KeyA").consumed, "muhenkan passes keys on");
}

#[test]
fn henkan_needs_a_profile() {
    let session = Session::with(&format!(r#"{{"harness": {HARNESS}}}"#));
    let field = session.raw();
    assert_eq!(field.error(r#"{"type":"select","mode":"henkan","profile":"main"}"#), "no-profile");
    let fresh = field.state();
    assert_eq!((fresh.mode, fresh.profile.as_deref()), (None, None), "no mode before the first select");
    let out = field.reply(r#"{"type":"select","mode":"muhenkan","profile":null}"#);
    assert_eq!((out.mode, out.profile.as_deref()), (Some(bada_compat::Mode::Muhenkan), None));
}

#[test]
fn flush_and_reset_leave_nothing_staged() {
    let session = Session::new();
    let field = session.field();
    field.stage();
    let out = field.reply(r#"{"type":"flush","cause":"blur"}"#);
    assert_eq!((out.commit.as_str(), out.preedit.text.as_str(), out.staged.as_str()), ("AB", "", ""));
    let out = field.reply(r#"{"type":"flush"}"#);
    assert_eq!((out.commit.as_str(), out.preedit.text.as_str()), ("", ""));
    field.stage();
    let out = field.reply(r#"{"type":"reset"}"#);
    assert_eq!(
        (out.commit.as_str(), out.preedit.text.as_str(), out.staged.as_str()),
        ("", "", ""),
        "reset commits nothing"
    );
    assert_eq!(field.reply(r#"{"type":"flush"}"#).commit, "", "and nothing was left behind");
}

#[test]
fn staged_is_what_flush_would_commit() {
    let session = Session::new();
    let field = session.field();
    assert_eq!(field.state().staged, "");
    field.stage();
    let moved = field.reply(r#"{"type":"action","name":"menu.next"}"#);
    assert_eq!(moved.staged, "ab", "the row that shows");
    assert_eq!(field.state().staged, moved.staged);
    assert_eq!(field.reply(r#"{"type":"flush"}"#).commit, moved.staged);
    assert_eq!(field.state().staged, "");
}

#[test]
fn state_changes_nothing() {
    let session = Session::new();
    let field = session.field();
    let shown = field.stage();
    let first = field.state();
    assert_eq!(first, field.state(), "sent any number of times, it answers the same");
    assert_eq!((&first.preedit, &first.menu, &first.staged), (&shown.preedit, &shown.menu, &shown.staged));
    assert_eq!((first.mode, first.profile.as_deref()), (Some(bada_compat::Mode::Henkan), Some("main")));
    let committed = field.key("Space");
    assert_eq!((committed.commit.as_str(), committed.consumed), ("AB", false));
    let after = field.state();
    assert_eq!((after.commit.as_str(), after.preedit.text.as_str()), ("", ""), "state has no commit of its own");
}

#[test]
fn an_unused_key_commits_and_passes_on() {
    let session = Session::new();
    let field = session.field();
    field.stage();
    let out = field.key("NoSuchKey");
    assert_eq!((out.commit.as_str(), out.preedit.text.as_str(), out.consumed), ("AB", "", false));
}

#[test]
fn positions_are_in_scalar_values() {
    let session = Session::new();
    let field = session.field();
    for out in [field.stage(), field.reply(r#"{"type":"pick","row":1}"#)] {
        let len = out.preedit.text.chars().count();
        assert_eq!(out.preedit.segments.iter().map(|s| s.len).sum::<usize>(), len);
        assert!(out.preedit.caret() <= len);
    }
    let picked = field.state();
    assert_eq!(picked.menu.selected, Some(1));
    assert_eq!(picked.preedit.segments[0].kind, bada_compat::SegmentKind::Picked);
}

#[test]
fn an_ask_is_answered_once() {
    let session = Session::new();
    let field = session.raw();
    field.reply(r#"{"type":"select","mode":"muhenkan","profile":null}"#);
    let asked = field.reply(r#"{"type":"action","name":"shout"}"#);
    assert_eq!(asked.ask, Some(bada_compat::Ask::Selection("shout".into())));
    let out = field.reply(r#"{"type":"text","selection":"quiet"}"#);
    assert_eq!((out.commit.as_str(), out.replace), ("QUIET", bada_compat::Replace::Selection));
    assert_eq!(
        field.reply(r#"{"type":"text","selection":"quiet"}"#).commit,
        "",
        "a text event nobody asked for changes nothing"
    );
}

#[test]
fn replies_live_until_the_next_send() {
    let session = Session::new();
    let field = session.field();
    let first = abi::send(session.0, field.engine, s(r#"{"type":"key","key":"KeyA"}"#).as_ptr());
    let copy = unsafe { CStr::from_ptr(first) }.to_owned();
    // A session event has its own reply: the engine's stays as it was.
    let on_session = abi::send(session.0, std::ptr::null_mut(), s(r#"{"type":"settings"}"#).as_ptr());
    assert_eq!(unsafe { CStr::from_ptr(first) }, copy.as_c_str());
    assert_eq!(read(on_session)["ok"], true);
    // The argument was copied: freeing it changes nothing.
    let arg = s(r#"{"type":"key","key":"KeyB"}"#);
    let second = abi::send(session.0, field.engine, arg.as_ptr());
    drop(arg);
    assert_eq!(read(second)["preedit"]["text"], "AB");
}

#[test]
fn engines_and_the_session_run_on_threads_at_once() {
    let session = Session::new();
    thread::scope(|scope| {
        for _ in 0..8 {
            scope.spawn(|| {
                let field = session.field();
                for _ in 0..200 {
                    field.stage();
                    assert_eq!(field.reply(r#"{"type":"flush"}"#).commit, "AB");
                }
            });
        }
        scope.spawn(|| {
            // The settings change under the fields, from another thread: the top row stays.
            for i in 0..200 {
                let forms = if i % 2 == 0 { json!(["upper", "lower"]) } else { json!(["upper", "title"]) };
                assert!(session.settings(json!({"type": "settings", "patch": {"forms": forms}})).ok);
            }
        });
    });
}

#[test]
fn a_panic_is_caught_and_the_engine_reset() {
    let session = Session::new();
    let field = session.field();
    field.stage();
    assert_eq!(field.error(r#"{"type":"key","key":"ShoutPanics"}"#), "panic");
    let after = field.state();
    assert_eq!((after.preedit.text.as_str(), after.staged.as_str()), ("", ""), "nothing is staged");
    assert_eq!(
        (after.mode, after.profile.as_deref()),
        (Some(bada_compat::Mode::Henkan), Some("main")),
        "still as selected"
    );
    assert_eq!(field.key("KeyC").preedit.text, "C", "and it types again, in henkan");
}

#[test]
fn bad_settings_change_nothing() {
    let session = Session::new();
    let before = session.settings(json!({"type": "settings"}));
    assert!(before.ok && before.problems.is_empty());
    for bad in [
        json!({"forms": []}),
        json!({"forms": ["upper", "upper"]}),
        json!({"forms": ["loud"]}),
        json!({"unknown": 1}),
        json!({"henkan": {"profiles": [{"id": "Main Profile", "name": "x"}]}}),
        json!({"henkan": {"profiles": [{"id": "a", "name": "x"}, {"id": "a", "name": "y"}]}}),
    ] {
        let reply = session.settings(json!({"type": "settings", "set": bad}));
        assert!(!reply.ok, "{bad}");
        assert_eq!(reply.error.as_ref().unwrap().code, "invalid");
        assert!(!reply.problems.is_empty());
        assert_eq!(reply.settings, before.settings, "what was in effect stays in effect");
    }
    let again = session.settings(json!({"type": "settings"}));
    assert_eq!(again.settings, before.settings);
    assert!(!again.problems.is_empty(), "re-reading answers the problems of the last change");
    // A good change clears them.
    assert!(session.settings(json!({"type": "settings", "patch": {"forms": ["title"]}})).ok);
    let now = session.settings(json!({"type": "settings"}));
    assert!(now.problems.is_empty());
    assert_eq!(now.settings.unwrap()["forms"], json!(["title"]));
}

#[test]
fn dry_changes_nothing() {
    let session = Session::new();
    let before = session.settings(json!({"type": "settings"})).settings;
    let dry = session.settings(json!({"type": "settings", "patch": {"forms": ["title"]}, "dry": true}));
    assert!(dry.ok);
    assert_eq!(dry.settings.unwrap()["forms"], json!(["title"]), "what it would be");
    assert_eq!(session.settings(json!({"type": "settings"})).settings, before);
    let dry = session.settings(json!({"type": "settings", "set": {"forms": []}, "dry": true}));
    assert!(!dry.ok);
    assert!(session.settings(json!({"type": "settings"})).problems.is_empty(), "a dry run leaves no problems behind");
}

#[test]
fn more_profiles_than_it_takes_are_refused() {
    let session = Session::new();
    let profile = |i: usize| json!({"id": format!("p{i}"), "name": "x"});
    let enough: Vec<Value> = (0..32).map(profile).collect();
    assert!(
        session.settings(json!({"type": "settings", "patch": {"henkan": {"profiles": enough}}})).ok,
        "32 are taken"
    );
    let many: Vec<Value> = (0..33).map(profile).collect();
    let reply = session.settings(json!({"type": "settings", "patch": {"henkan": {"profiles": many}}}));
    assert!(!reply.ok);
    assert_eq!(reply.problems[0].code, "too-many-profiles");
    assert_eq!(reply.settings.unwrap()["henkan"]["profiles"].as_array().unwrap().len(), 32, "never truncated: refused");
}

#[test]
fn a_session_needs_a_harness() {
    for init in ["", "{", "{}", r#"{"settings": {}}"#, r#"{"harness": {"id": "x"}}"#] {
        assert!(abi::session_new(std::ptr::null(), s(init).as_ptr()).is_null(), "{init:?}");
    }
    assert!(abi::session_new(std::ptr::null(), std::ptr::null()).is_null());
    // Settings that are wrong don't fail the session: it starts with the defaults.
    let session = Session::with(&format!(r#"{{"harness": {HARNESS}, "settings": {{"forms": []}}}}"#));
    let read = session.settings(json!({"type": "settings"}));
    assert_eq!(read.settings.unwrap()["forms"], json!(["upper", "lower"]));
    assert_eq!(read.problems.len(), 1, "and what was wrong can be read back");
}

#[test]
fn null_handles_are_harmless() {
    assert!(abi::send(std::ptr::null_mut(), std::ptr::null_mut(), s(r#"{"type":"state"}"#).as_ptr()).is_null());
    assert!(abi::new(std::ptr::null_mut()).is_null());
    abi::free(std::ptr::null_mut());
    abi::session_free(std::ptr::null_mut());
    abi::data_free(std::ptr::null());
}

#[test]
fn data_takes_named_buffers() {
    let name = s("no-such-buffer");
    let bytes = b"ignored";
    let bufs = [abi::PBuf { name: name.as_ptr(), ptr: bytes.as_ptr(), len: bytes.len(), flags: abi::P_BORROWED }];
    let data = abi::data_new(bufs.as_ptr(), bufs.len());
    assert!(!data.is_null(), "a name the engine doesn't know is ignored");
    let session = abi::session_new(data, s(&format!(r#"{{"harness": {HARNESS}}}"#)).as_ptr());
    assert!(!session.is_null());
    abi::session_free(session);
    abi::data_free(data);
    let none = abi::data_new(std::ptr::null(), 0);
    assert!(!none.is_null());
    abi::data_free(none);
}

#[test]
fn the_header_names_the_eight_functions() {
    let header = bada_compat::header("shout");
    for name in ["describe", "data_new", "data_free", "session_new", "session_free", "new", "free", "send"] {
        assert!(header.contains(&format!("shout_{name}(")), "{name}");
    }
    assert!(header.contains("SHOUT_BORROWED 1u"));
}

#[test]
fn the_version_is_the_repositorys() {
    let version = include_str!("../../../VERSION").trim();
    assert_eq!(env!("CARGO_PKG_VERSION"), version, "Cargo.toml's workspace version and the VERSION file");
}
