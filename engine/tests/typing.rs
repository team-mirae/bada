//! bada, typed on through its C ABI: what bada ime 0.3.1 did with these keys
//! (`docs/reference/bada-ime-spec.md` §6), with every layout read from its file.

use std::ffi::{CStr, CString};

use bada::ffi as abi;
use bada_compat::{Ask, Replace, Reply, SegmentKind, SessionReply};
use serde_json::{Value, json};

const HARNESS: &str = r#"{"id": "com.teammirae.bada.test", "version": "0.1.0", "features": ["segments", "pick"]}"#;

struct Session(*mut abi::Session, *const abi::Data);

impl Session {
    /// A session with these profiles (id → layouts), and QWERTY in muhenkan.
    fn new(profiles: Value) -> Session {
        let profiles: Vec<Value> = profiles
            .as_object()
            .unwrap()
            .iter()
            .map(|(id, p)| {
                let mut profile = if p.is_array() { json!({"layouts": p}) } else { p.clone() };
                profile["id"] = json!(id);
                profile["name"] = json!(id);
                profile
            })
            .collect();
        Session::with(json!({"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": profiles}}))
    }

    fn with(settings: Value) -> Session {
        Session::with_data(settings, &[])
    }

    /// A session on data: named buffers, as a harness passes them.
    fn with_data(settings: Value, buffers: &[(&str, &[u8])]) -> Session {
        let names: Vec<CString> = buffers.iter().map(|b| CString::new(b.0).unwrap()).collect();
        let bufs: Vec<abi::PBuf> = buffers
            .iter()
            .zip(&names)
            .map(|(b, n)| abi::PBuf { name: n.as_ptr(), ptr: b.1.as_ptr(), len: b.1.len(), flags: 0 })
            .collect();
        let data = if buffers.is_empty() { std::ptr::null() } else { abi::data_new(bufs.as_ptr(), bufs.len()) };
        let init = format!(r#"{{"harness": {HARNESS}, "settings": {settings}}}"#);
        let session = abi::session_new(data, CString::new(init).unwrap().as_ptr());
        assert!(!session.is_null());
        let made = Session(session, data);
        let read = made.event(json!({"type": "settings"}));
        assert!(read.problems.is_empty(), "{:?}", read.problems);
        made
    }

    fn event(&self, event: Value) -> SessionReply {
        let text = CString::new(event.to_string()).unwrap();
        let reply = abi::send(self.0, std::ptr::null_mut(), text.as_ptr());
        serde_json::from_str(unsafe { CStr::from_ptr(reply) }.to_str().unwrap()).expect("a session reply")
    }

    /// A text field, in henkan with `profile`, or in muhenkan with none.
    fn field(&self, profile: Option<&str>) -> Field<'_> {
        let field = Field { session: self, engine: abi::new(self.0) };
        let mode = if profile.is_some() { "henkan" } else { "muhenkan" };
        field.ok(json!({"type": "select", "mode": mode, "profile": profile}));
        field
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        abi::session_free(self.0);
        abi::data_free(self.1);
    }
}

struct Field<'a> {
    session: &'a Session,
    engine: *mut abi::Engine,
}

impl Field<'_> {
    fn send(&self, event: Value) -> Value {
        let text = CString::new(event.to_string()).unwrap();
        let reply = abi::send(self.session.0, self.engine, text.as_ptr());
        serde_json::from_str(unsafe { CStr::from_ptr(reply) }.to_str().unwrap()).expect("JSON")
    }

    fn ok(&self, event: Value) -> Reply {
        let value = self.send(event.clone());
        assert_eq!(value["ok"], true, "{event}: {value}");
        serde_json::from_value(value).expect("a reply")
    }

    fn error(&self, event: Value) -> String {
        let value = self.send(event.clone());
        assert_eq!(value["ok"], false, "{event}: {value}");
        value["error"]["code"].as_str().unwrap().to_string()
    }

    /// One key: `"KeyA"`, or with modifiers `"shift+KeyA"`, `"alt+shift+Digit1"`.
    fn key(&self, chord: &str) -> Reply {
        let mut event = json!({"type": "key"});
        let mut parts: Vec<&str> = chord.split('+').collect();
        event["key"] = json!(parts.pop().unwrap());
        for m in parts {
            event[m] = json!(true);
        }
        self.ok(event)
    }

    /// Types text on the keys QWERTY has it on (capitals and shifted symbols with ⇧), and returns
    /// the last reply. Everything committed on the way is collected in `commit`.
    fn typed(&self, text: &str) -> Reply {
        let mut commit = String::new();
        let mut last = Reply::default();
        for c in text.chars() {
            let (code, shift) = key_for(c);
            last = self.key(&if shift { format!("shift+{code}") } else { code });
            commit.push_str(&last.commit);
        }
        last.commit = commit;
        last
    }

    fn action(&self, name: &str) -> Reply {
        self.ok(json!({"type": "action", "name": name}))
    }

    fn flush(&self) -> String {
        self.ok(json!({"type": "flush"})).commit
    }
}

impl Drop for Field<'_> {
    fn drop(&mut self) {
        abi::free(self.engine);
    }
}

fn key_for(c: char) -> (String, bool) {
    if c.is_ascii_alphabetic() {
        return (format!("Key{}", c.to_ascii_uppercase()), c.is_ascii_uppercase());
    }
    if c == ' ' {
        return ("Space".into(), false);
    }
    for key in &bada_compat::Key::ALL[..48] {
        for shift in [false, true] {
            if bada::layout::jis(*key, shift) == Some(c) {
                return (key.code().to_string(), shift);
            }
        }
    }
    panic!("no key types {c:?}");
}

fn rows(reply: &Reply) -> Vec<&str> {
    reply.menu.rows.iter().map(|r| r.text.as_str()).collect()
}

// ---------------------------------------------------------------- muhenkan

#[test]
fn muhenkan_says_what_each_key_types() {
    let session = Session::new(json!({}));
    let field = session.field(None);
    let a = field.key("KeyA");
    assert_eq!((a.consumed, a.types.as_deref(), a.preedit.text.as_str()), (false, Some("a"), ""));
    assert_eq!(field.key("shift+KeyA").types.as_deref(), Some("A"));
    assert_eq!(field.key("IntlRo").types.as_deref(), Some("\\"), "the JIS legend, whatever the OS thinks");
    assert_eq!(field.key("shift+IntlRo").types.as_deref(), Some("_"));
    assert_eq!(field.key("alt+Space").types.as_deref(), Some("_"));
    let empty = field.key("shift+Digit0");
    assert_eq!((empty.consumed, empty.types), (true, None), "⇧0 is empty: it types nothing");
    for other in ["Space", "Enter", "ArrowLeft", "F7", "meta+KeyC", "ctrl+KeyC", "NoSuchKey"] {
        let out = field.key(other);
        assert_eq!((out.consumed, out.types), (false, None), "{other}");
    }
}

#[test]
fn muhenkan_follows_its_layout() {
    let session = Session::with(json!({"muhenkan": {"layout": "en-dvorak"}}));
    let field = session.field(None);
    assert_eq!(field.key("KeyJ").types.as_deref(), Some("h"));
    assert_eq!(field.key("shift+Digit0").types.as_deref(), Some("~"));
    assert_eq!(field.key("meta+KeyC").types, None, "shortcuts follow the QWERTY positions");
    // With no muhenkan layout, as on a new install, every key is passed on.
    let unset = Session::with(json!({}));
    let field = unset.field(None);
    let out = field.key("KeyA");
    assert_eq!((out.consumed, out.types), (false, None));
}

#[test]
fn variants_open_in_a_strip() {
    let session = Session::new(json!({}));
    let field = session.field(None);
    let open = field.key("alt+KeyE");
    assert!(open.consumed && open.menu.strip);
    assert_eq!((open.preedit.text.as_str(), open.menu.selected, open.staged.as_str()), ("e", None, "e"));
    assert_eq!(rows(&open), ["è", "é", "ê", "ë", "ē", "ė", "ę"]);
    assert_eq!(field.key("Digit2").commit, "é", "a digit picks");
    field.key("alt+KeyE");
    assert_eq!(field.key("alt+KeyE").preedit.text, "è", "the same key again: the next variant");
    assert_eq!(field.key("ArrowRight").preedit.text, "é");
    assert_eq!(field.ok(json!({"type": "pick", "row": 3})).preedit.text, "ë");
    assert_eq!(field.key("Enter").commit, "ë");
    field.key("alt+shift+KeyE");
    let cancelled = field.key("Escape");
    assert_eq!((cancelled.commit.as_str(), cancelled.preedit.text.as_str(), cancelled.consumed), ("", "", true));
    field.key("alt+KeyU");
    let on = field.key("KeyR");
    assert_eq!(
        (on.commit.as_str(), on.types.as_deref(), on.consumed),
        ("u", Some("r"), false),
        "any other key keeps what shows"
    );
    assert_eq!(field.key("alt+KeyK").types, None, "a letter without variants is the app's");
    field.key("alt+KeyA");
    assert_eq!(field.flush(), "a", "staged is what shows");
}

// ---------------------------------------------------------------- henkan

#[test]
fn korean_composes_and_commits_then_acts() {
    let session = Session::new(json!({"main": ["ko-2-set"]}));
    let field = session.field(Some("main"));
    let out = field.typed("gksrnrdj");
    assert_eq!((out.preedit.text.as_str(), out.staged.as_str(), out.consumed), ("한국어", "한국어", true));
    assert_eq!(out.preedit.segments.len(), 1);
    assert!(out.menu.rows.is_empty(), "one reading: no menu");
    assert_eq!(field.key("Backspace").preedit.text, "한국ㅇ", "the last key is taken back, and the rest read again");
    field.key("KeyJ");
    let space = field.key("Space");
    assert_eq!((space.commit.as_str(), space.consumed, space.preedit.text.as_str()), ("한국어", false, ""));
    field.typed("gks");
    let esc = field.key("Escape");
    assert_eq!((esc.commit.as_str(), esc.consumed), ("한", false), "Esc commits: it doesn't cancel");
    field.typed("gks");
    let enter = field.key("shift+Enter");
    assert_eq!((enter.commit.as_str(), enter.consumed), ("한", true), "⇧Enter commits and does nothing else");
    field.typed("gks");
    let shortcut = field.key("meta+KeyS");
    assert_eq!((shortcut.commit.as_str(), shortcut.consumed, shortcut.types), ("한", false, None));
    // With nothing staged, keys go to the app.
    for key in ["Space", "Enter", "Tab", "Backspace", "shift+Enter"] {
        assert!(!field.key(key).consumed, "{key}");
    }
    assert_eq!(field.typed("¥").preedit.text, "₩", "the Korean layouts type ₩ where ¥ is");
    let alt_space = field.key("alt+Space");
    assert_eq!((alt_space.commit.as_str(), alt_space.consumed), ("₩\u{3000}", true), "⌥Space: a full-width space");
}

#[test]
fn a_modifier_alone_is_no_key_press() {
    let session = Session::new(json!({"main": ["ko-2-set"]}));
    let field = session.field(Some("main"));
    field.typed("gk");
    for key in ["ShiftLeft", "AltRight", "MetaLeft", "ControlLeft", "CapsLock"] {
        let out = field.key(key);
        assert_eq!((out.commit.as_str(), out.preedit.text.as_str(), out.consumed), ("", "하", false), "{key}");
    }
    assert_eq!(field.typed("s").preedit.text, "한", "and composition goes on");
    // Every reply writes its caret, an empty one too.
    let raw = field.send(json!({"type": "reset"}));
    assert_eq!(raw["preedit"], json!({"text": "", "caret": 0, "segments": []}));
    // The session's layouts are listed in the order describe has them.
    let listed: Vec<String> =
        session.event(json!({"type": "layout"})).layouts.unwrap().into_iter().map(|l| l.id).collect();
    assert_eq!(&listed[..3], ["en-qwerty", "en-qwerty-us", "en-dvorak"]);
}

#[test]
fn an_empty_slot_commits_and_types_nothing() {
    let session = Session::new(json!({"main": ["ko-2-set"]}));
    let field = session.field(Some("main"));
    field.typed("gks");
    let out = field.key("shift+Digit0");
    assert_eq!((out.commit.as_str(), out.consumed, out.types), ("한", true, None));
    field.typed("gks");
    let out = field.key("alt+KeyA");
    assert_eq!((out.commit.as_str(), out.consumed), ("한", false), "⌥ + a key isn't the layout's: it goes to the app");
}

#[test]
fn japanese_reads_and_changes_script() {
    let session = Session::new(
        json!({"ja": {"layouts": ["ja-romaji"], "script_forms": true}, "plain": ["ja-romaji"], "kana": ["ja-kana"]}),
    );
    let field = session.field(Some("ja"));
    let out = field.typed("konnichiha");
    assert_eq!(out.preedit.text, "こんにちは");
    // The layout's forms below the top entry: −1 hiragana, −2 katakana.
    let entries: Vec<(i32, &str)> = out.menu.rows.iter().map(|r| (r.entry, r.text.as_str())).collect();
    assert_eq!(entries, [(-2, "コンニチハ"), (-1, "こんにちは"), (0, "こんにちは")]);
    assert_eq!(out.menu.selected, Some(2));
    assert_eq!(out.menu.rows[2].lang.as_deref(), Some("ja"));
    assert_eq!(field.key("shift+Tab").preedit.text, "こんにちは");
    let kata = field.key("shift+Tab");
    assert_eq!(
        (kata.preedit.text.as_str(), kata.menu.selected, kata.preedit.segments[0].kind),
        ("コンニチハ", Some(0), SegmentKind::Picked)
    );
    assert_eq!(field.key("shift+Tab").menu.selected, Some(0), "no wrap");
    assert_eq!(field.flush(), "コンニチハ");
    // Off, there are no negative entries; the actions still change script.
    let field = session.field(Some("plain"));
    assert!(field.typed("ka").menu.rows.is_empty());
    assert_eq!(field.action("ja-romaji.katakana").preedit.text, "カ");
    assert_eq!(field.action("ja-romaji.halfwidth-katakana").preedit.text, "ｶ");
    assert_eq!(field.action("ja-romaji.fullwidth-latin").preedit.text, "ｋａ");
    assert_eq!(field.action("ja-romaji.halfwidth-latin").preedit.text, "ka");
    assert_eq!(field.typed("n").preedit.text, "kan", "it lasts until the commit");
    assert_eq!(field.flush(), "kan");
    assert_eq!(field.typed("ka").preedit.text, "か");
    assert_eq!(
        field.error(json!({"type": "action", "name": "ja-kana.katakana"})),
        "unknown-action",
        "its layout isn't in this profile"
    );
    field.flush();
    // The half-width form of what romaji types full-width is a row.
    let digits = field.typed("nihon1");
    assert_eq!(rows(&digits), ["にほん１", "にほん1"]);
    field.flush();
    // JIS kana: marks attach, and ⌥⇧ types the shifted character, full-width, staged with the kana.
    let field = session.field(Some("kana"));
    for key in ["KeyT", "BracketLeft", "shift+KeyZ", "KeyB", "Digit4"] {
        field.key(key);
    }
    assert_eq!(field.key("alt+shift+Slash").preedit.text, "がっこう？");
    assert_eq!(field.key("alt+Slash").commit, "がっこう？", "⌥ without ⇧ isn't the layout's");
}

#[test]
fn text_ends_the_word_in_bada_roman() {
    let session = Session::new(json!({"main": ["ko-bada-roman"], "mixed": ["ko-bada-roman", "ja-kana"]}));
    let field = session.field(Some("main"));
    assert_eq!(field.typed("hanguk").preedit.text, "한국");
    let stop = field.key("Period");
    assert_eq!((stop.commit.as_str(), stop.consumed, stop.preedit.text.as_str()), ("한국.", true, ""));
    assert_eq!(field.typed("han;guk").preedit.text, "한국", "; is a break, and is staged");
    assert_eq!(field.key("shift+Semicolon").commit, "한국;");
    assert_eq!(field.typed("pe-").preedit.text, "패", "- after e is a letter");
    assert_eq!(field.typed("-").commit, "패-", "and a hyphen elsewhere");
    assert_eq!(field.key("alt+Semicolon").commit, "·");
    assert_eq!(field.key("alt+shift+BracketRight").commit, "{");
    assert_eq!(field.typed("1").commit, "1", "with nothing staged it is just typed");
    // Beside a layout that reads the key as more than text, it is staged as usual.
    let field = session.field(Some("mixed"));
    let out = field.typed("3");
    assert_eq!((out.commit.as_str(), out.consumed), ("", true));
    assert!(rows(&out).contains(&"あ"));
}

#[test]
fn the_menu_picks_and_typing_on_fixes() {
    let session = Session::new(json!({"work": ["ko-2-set", "en-qwerty"]}));
    let field = session.field(Some("work"));
    let out = field.typed("gks");
    assert_eq!(rows(&out), ["한", "gks"]);
    assert_eq!(out.menu.rows[1].layout.as_deref(), Some("en-qwerty"));
    let picked = field.action("menu.next");
    assert_eq!((picked.preedit.text.as_str(), picked.menu.selected, picked.staged.as_str()), ("gks", Some(1), "gks"));
    assert_eq!(field.action("menu.next").menu.selected, Some(1), "no wrap");
    // Typing on fixes the picked row, and its layout sticks.
    let on = field.typed("rk");
    assert_eq!(on.preedit.text, "gksrk");
    let kinds: Vec<_> = on.preedit.segments.iter().map(|s| (s.len, s.kind)).collect();
    assert_eq!(kinds, [(3, SegmentKind::Fixed), (2, SegmentKind::Picked)]);
    assert_eq!(rows(&on), ["가", "rk"], "the rows are the readings of the keys after the fix");
    // Backspace takes back the keys after the fix, then the fixed text a character at a time.
    field.key("Backspace");
    assert_eq!(field.key("Backspace").preedit.text, "gks");
    assert_eq!(field.key("Backspace").preedit.text, "gk");
    assert_eq!(field.flush(), "gk");
    // Tab and the arrows move too; pick goes to a row.
    field.typed("gks");
    assert_eq!(field.key("Tab").preedit.text, "gks");
    assert_eq!(field.key("ArrowUp").preedit.text, "한");
    assert_eq!(field.ok(json!({"type": "pick", "row": 1})).preedit.text, "gks");
    assert_eq!(field.error(json!({"type": "pick", "row": 2})), "bad-row");
    assert_eq!(field.flush(), "gks", "committing commits the entry shown");
}

#[test]
fn fix_keeps_what_shows() {
    let session = Session::new(json!({"main": ["ja-romaji"]}));
    let field = session.field(Some("main"));
    field.typed("anata");
    let fixed = field.action("fix");
    assert_eq!((fixed.preedit.text.as_str(), fixed.commit.as_str(), fixed.staged.as_str()), ("あなた", "", "あなた"));
    assert_eq!(fixed.preedit.segments[0].kind, SegmentKind::Fixed);
    let on = field.typed("ha");
    assert_eq!(on.preedit.text, "あなたは");
    assert_eq!(on.preedit.segments.len(), 2);
    assert_eq!(field.flush(), "あなたは");
    assert!(field.action("fix").consumed, "with nothing staged, nothing");
}

#[test]
fn width_acts_on_what_is_staged_or_else_the_selection() {
    let session = Session::new(json!({"main": ["ko-bada-roman"], "en": ["en-qwerty"]}));
    let field = session.field(Some("en"));
    field.typed("ab1");
    assert_eq!(field.action("width.full").preedit.text, "ａｂ１");
    assert_eq!(field.typed("c").preedit.text, "ａｂ１c", "it stays staged in that width, and typing goes on after it");
    assert_eq!(field.action("width.half").preedit.text, "ab1c");
    field.flush();
    // Nothing staged: the selection, which the engine asks for.
    let asked = field.action("width.full");
    assert_eq!(asked.ask, Some(Ask::Selection("full".into())));
    let made = field.ok(json!({"type": "text", "selection": "han 12"}));
    assert_eq!((made.commit.as_str(), made.replace), ("ｈａｎ\u{3000}１２", Replace::Selection));
    assert_eq!(field.ok(json!({"type": "text", "selection": "again"})).commit, "", "asked once");
    // The same in muhenkan.
    let field = session.field(None);
    assert_eq!(field.action("width.half").ask, Some(Ask::Selection("half".into())));
    assert_eq!(field.ok(json!({"type": "text", "selection": "ｈａｎ"})).commit, "han");
}

#[test]
fn jamo_actions_keep_one_jamo_of_each_syllable() {
    let session = Session::new(json!({"main": ["ko-bada-roman"]}));
    let field = session.field(Some("main"));
    field.typed("hanguk");
    assert_eq!(field.action("ko-bada-roman.initials").preedit.text, "ㅎㄱ");
    assert_eq!(field.flush(), "ㅎㄱ");
    field.typed("dalgogi");
    assert_eq!(field.action("ko-bada-roman.finals").preedit.text, "ㄹ");
    field.flush();
    let asked = field.action("ko-bada-roman.vowels");
    assert_eq!(asked.ask, Some(Ask::Selection("vowels".into())));
    let made = field.ok(json!({"type": "text", "selection": "과자!"}));
    assert_eq!((made.commit.as_str(), made.replace), ("ㅘㅏ!", Replace::Selection));
    assert_eq!(field.typed("ha").preedit.text, "하");
    assert_eq!(field.action("ko-bada-roman.fullwidth-latin").preedit.text, "ｈａ");
    assert_eq!(field.error(json!({"type": "action", "name": "ko-bada-roman.shout"})), "unknown-action");
}

#[test]
fn f_keys_are_each_profiles_own() {
    let session = Session::new(json!({
        "ja": {"layouts": ["ja-kana"], "fkeys": {"intercept": true, "keys": {"F7": "ja-kana.katakana", "F8": "pass", "F9": "ko-2-set.hanja"}}},
        "open": {"layouts": ["ja-kana"], "fkeys": {"intercept": false}},
        "plain": ["ja-kana"],
    }));
    let field = session.field(Some("ja"));
    field.key("KeyT");
    let f7 = field.key("F7");
    assert_eq!((f7.preedit.text.as_str(), f7.consumed), ("カ", true));
    assert!(field.key("F6").consumed, "unbound: swallowed");
    assert!(field.key("shift+F7").consumed, "a modified F key is swallowed");
    assert!(field.key("F9").consumed, "an action of a layout that isn't in the profile: swallowed");
    let f8 = field.key("F8");
    assert_eq!((f8.consumed, f8.commit.as_str(), f8.preedit.text.as_str()), (false, "", "カ"), "pass");
    let f5 = field.key("F5");
    assert_eq!(
        (f5.consumed, f5.commit.as_str()),
        (false, "カ"),
        "only F6 to F10 are the engine's: any other commits and passes"
    );
    assert!(!session.field(Some("open")).key("F7").consumed, "not intercepted: every F key goes to the app");
    assert!(session.field(Some("plain")).key("F7").consumed, "no fkeys: intercepted, none bound");
    assert!(!session.field(None).key("F7").consumed, "muhenkan passes F keys on");
}

#[test]
fn search_layouts_take_space() {
    let session = Session::new(json!({"sym": ["uni-symbols", "uni-emojis"], "mixed": ["uni-symbols", "ko-2-set"]}));
    let field = session.field(Some("sym"));
    let out = field.typed("Happy face");
    assert_eq!((out.preedit.text.as_str(), out.commit.as_str()), ("happy face", ""), "Space is part of the search");
    let done = field.key("shift+Space");
    assert_eq!(
        (done.commit.as_str(), done.consumed),
        ("happy face", false),
        "⇧Space commits, and the app types the space"
    );
    let field = session.field(Some("mixed"));
    field.typed("gk");
    assert_eq!(field.key("Space").commit, "하", "beside other layouts Space commits as usual");
}

// ---------------------------------------------------------------- select, settings, layouts

#[test]
fn select_commits_and_refuses_what_isnt_there() {
    let session = Session::new(json!({"main": ["ko-2-set"], "ja": ["ja-romaji"]}));
    let field = session.field(Some("main"));
    field.typed("gks");
    let same = field.ok(json!({"type": "select", "mode": "henkan", "profile": "main"}));
    assert_eq!((same.commit.as_str(), same.preedit.text.as_str()), ("", "한"));
    let other = field.ok(json!({"type": "select", "mode": "henkan", "profile": "ja"}));
    assert_eq!((other.commit.as_str(), other.profile.as_deref()), ("한", Some("ja")));
    assert_eq!(field.typed("ka").preedit.text, "か");
    assert_eq!(field.ok(json!({"type": "select", "mode": "muhenkan", "profile": "ja"})).commit, "か");
    assert_eq!(field.error(json!({"type": "select", "mode": "henkan", "profile": "nobody"})), "unknown-profile");
    let empty = Session::with(json!({}));
    assert_eq!(empty.field(None).error(json!({"type": "select", "mode": "henkan", "profile": "main"})), "no-profile");
}

#[test]
fn a_changed_profile_commits_at_the_next_event() {
    let session = Session::new(json!({"main": ["ko-2-set"], "ja": ["ja-romaji"]}));
    let field = session.field(Some("main"));
    let other = session.field(Some("ja"));
    field.typed("gks");
    other.typed("ka");
    // main changes; ja doesn't.
    let patch = json!({"henkan": {"profiles": [{"id": "main", "name": "main", "layouts": ["ko-bada-roman"]}, {"id": "ja", "name": "ja", "layouts": ["ja-romaji"]}]}});
    assert!(session.event(json!({"type": "settings", "patch": patch})).ok);
    let next = field.typed("h");
    assert_eq!(
        (next.commit.as_str(), next.preedit.text.as_str()),
        ("한", "ㅎ"),
        "what was staged is committed, then the key is read by the new profile"
    );
    assert_eq!(other.typed("n").preedit.text, "かん", "an unchanged profile goes on");
    // main is removed.
    field.typed("a");
    let patch = json!({"henkan": {"profiles": [{"id": "ja", "name": "ja", "layouts": ["ja-romaji"]}]}});
    assert!(session.event(json!({"type": "settings", "patch": patch})).ok);
    assert_eq!(field.flush(), "하", "it commits what it had");
    assert_eq!(field.error(json!({"type": "key", "key": "KeyA"})), "no-profile");
    assert_eq!(field.ok(json!({"type": "select", "mode": "henkan", "profile": "ja"})).profile.as_deref(), Some("ja"));
    assert_eq!(field.typed("a").preedit.text, "あ");
}

#[test]
fn bad_settings_change_nothing_and_say_why() {
    let session = Session::new(json!({"main": ["ko-2-set"]}));
    let before = session.event(json!({"type": "settings"})).settings;
    let code = |settings: Value| {
        let reply = session.event(json!({"type": "settings", "set": settings}));
        assert!(!reply.ok);
        assert_eq!(reply.settings, before, "what was in effect stays in effect");
        (reply.problems[0].code.clone(), reply.problems[0].path.clone())
    };
    let profile = |layouts: Value| json!({"henkan": {"profiles": [{"id": "a", "name": "a", "layouts": layouts}]}});
    assert_eq!(code(profile(json!(["ko-3-set"]))), ("unknown-layout".into(), "henkan.profiles[0].layouts[0]".into()));
    assert_eq!(code(profile(json!([]))).0, "bad-value");
    assert_eq!(code(json!({"muhenkan": {"layout": "ko-2-set"}})).0, "not-muhenkan");
    assert_eq!(code(json!({"muhenkan": {"layout": 0}})).0, "bad-value");
    assert_eq!(code(json!({"enabled": [16]})).0, "unknown-field");
    assert_eq!(code(json!({"henkan": {"defaults": {"options": {"uni-emojis": {"tones": []}}}}})).0, "unknown-option");
    assert_eq!(
        code(json!({"henkan": {"defaults": {"options": {"uni-emojis": {"skin_tones": ["purple"]}}}}})).0,
        "bad-value"
    );
    assert_eq!(code(json!({"henkan": {"profiles": [{"id": "a", "name": "a", "layouts": ["ja-kana"], "fkeys": {"keys": {"F7": "ja-kana.romaji"}}}]}})).0, "unknown-action");
    let many: Vec<Value> =
        (0..=bada::PROFILES).map(|i| json!({"id": format!("p{i}"), "name": "x", "layouts": ["ko-2-set"]})).collect();
    assert_eq!(code(json!({"henkan": {"profiles": many}})).0, "too-many-profiles");
    let taken: Vec<Value> =
        (0..32).map(|i| json!({"id": format!("p{i}"), "name": "x", "layouts": ["ko-2-set"]})).collect();
    assert!(session.event(json!({"type": "settings", "set": {"henkan": {"profiles": taken}}})).ok, "32 are taken");
}

#[test]
fn a_custom_layout_is_a_file_like_any_other() {
    let session = Session::new(json!({"main": ["ko-2-set"]}));
    let listed = session.event(json!({"type": "layout"})).layouts.unwrap();
    assert_eq!(listed.len(), 11);
    assert!(listed.iter().all(|l| l.stock && !l.modified));
    // A changed copy of a stock layout replaces it, with the ranking warning.
    let mut mine: Value = serde_json::from_str(include_str!("../layouts/ko-2-set.json")).unwrap();
    mine["keys"]["Digit1"] = json!({"base": {"t": "ㅃ", "r": "i"}});
    let dry = session.event(json!({"type": "layout", "value": mine, "dry": true}));
    assert!(dry.ok && dry.problems[0].code == "stock-modified");
    let field = session.field(Some("main"));
    assert_eq!(field.typed("1k").preedit.text, "1ㅏ", "a dry run changes nothing");
    field.flush();
    let set = session.event(json!({"type": "layout", "value": mine}));
    assert!(set.ok);
    assert_eq!((set.problems[0].code.as_str(), set.problems[0].level), ("stock-modified", bada_compat::Level::Warning));
    let two_set = set.layouts.unwrap().into_iter().find(|l| l.id == "ko-2-set").unwrap();
    assert!(two_set.stock && two_set.modified);
    assert_eq!(field.typed("1k").preedit.text, "빠", "every field reads the new file");
    field.flush();
    // Removing it brings the stock one back.
    assert!(session.event(json!({"type": "layout", "id": "ko-2-set", "value": null})).ok);
    assert_eq!(field.typed("1k").preedit.text, "1ㅏ");
    field.flush();
    // A new id is a new layout: no key model.
    mine["id"] = json!("ko-my-2-set");
    let added = session.event(json!({"type": "layout", "value": mine}));
    assert_eq!(added.problems[0].code, "no-model");
    assert_eq!(added.layouts.as_ref().unwrap().len(), 12);
    assert!(!added.layouts.unwrap().iter().find(|l| l.id == "ko-my-2-set").unwrap().stock);
    assert!(session.event(json!({"type": "settings", "patch": {"henkan": {"profiles": [{"id": "main", "name": "m", "layouts": ["ko-my-2-set"]}]}}})).ok);
    assert_eq!(field.typed("1k").preedit.text, "빠");
    // What is wrong isn't loaded.
    mine["keys"]["KeyA"] = json!({"base": {"t": "a", "r": "l"}});
    let refused = session.event(json!({"type": "layout", "value": mine}));
    assert!(!refused.ok);
    assert_eq!(
        (refused.problems[0].code.as_str(), refused.problems[0].path.as_str()),
        ("bad-role", "keys.KeyA.base.r")
    );
    assert_eq!(session.event(json!({"type": "layout"})).problems[0].code, "bad-role", "and can be read back");
    assert!(!session.event(json!({"type": "layout", "id": "ko-nothing", "value": null})).ok);
    // A layout the settings in effect name can still be removed: the folder is the truth. The
    // settings stay in effect without it, and say what is wrong with them now.
    field.flush();
    let named = session.event(json!({"type": "layout", "id": "ko-my-2-set", "value": null}));
    assert!(named.ok);
    assert_eq!((named.problems[0].code.as_str(), named.problems[0].level), ("in-use", bada_compat::Level::Warning));
    assert_eq!(named.layouts.unwrap().len(), 11);
    let settings = session.event(json!({"type": "settings"}));
    assert_eq!(settings.problems[0].code, "unknown-layout");
    assert_eq!(
        settings.settings.unwrap()["henkan"]["profiles"][0]["layouts"],
        json!(["ko-my-2-set"]),
        "still in effect"
    );
    let out = field.typed("1");
    assert_eq!(
        (out.commit.as_str(), out.consumed, out.preedit.text.as_str()),
        ("", true, ""),
        "its profile has no layout left: nothing is typed"
    );
    // Settings that stand again clear the problem.
    assert!(session.event(json!({"type": "settings", "patch": {"henkan": {"profiles": [{"id": "main", "name": "m", "layouts": ["ko-2-set"]}]}}})).ok);
    assert!(session.event(json!({"type": "settings"})).problems.is_empty());
}

#[test]
fn the_dictionary_is_the_sessions() {
    let session = Session::new(json!({}));
    let set = session.event(json!({"type": "dictionary", "value": {"format": "bada-dictionary", "version": 1,
        "positive": [{"word": "데이터센터"}, {"word": "サーバー", "language": "japanese"}], "negative": [{"word": "감소합니다"}]}}));
    assert_eq!(set.dictionary.map(|d| (d.positive, d.negative)), Some((2, 1)));
    assert!(!session.event(json!({"type": "dictionary", "value": {"format": "other"}})).ok);
    assert_eq!(
        session.event(json!({"type": "dictionary"})).dictionary.unwrap().positive,
        2,
        "a bad one changes nothing"
    );
    assert_eq!(session.event(json!({"type": "dictionary", "value": null})).dictionary.unwrap().positive, 0);
}

#[test]
fn describe_lists_the_stock_eleven() {
    let text = unsafe { CStr::from_ptr(abi::describe()) }.to_str().unwrap();
    let d: Value = serde_json::from_str(text).unwrap();
    assert_eq!(d["engine"]["id"], "com.teammirae.bada");
    assert_eq!(d["engine"]["version"], include_str!("../../VERSION").trim());
    assert_eq!(d["layouts"].as_array().unwrap().len(), 11);
    assert_eq!(d["events"].as_array().unwrap().len(), 11);
    assert!(d["settings"]["schema"]["properties"]["henkan"].is_object());
    let kana = d["layouts"].as_array().unwrap().iter().find(|l| l["id"] == "ja-kana").unwrap();
    assert_eq!(
        (kana["kind"].as_str(), kana["read"].as_str(), kana["lang"].as_str()),
        (Some("henkan"), Some("kana"), Some("ja"))
    );
    assert!(kana["actions"].as_array().unwrap().iter().any(|a| a["id"] == "katakana"));
    let emoji = d["layouts"].as_array().unwrap().iter().find(|l| l["id"] == "uni-emojis").unwrap();
    assert_eq!(emoji["options"]["skin_tones"]["type"], "order");
}

#[test]
fn the_header_is_up_to_date() {
    assert_eq!(include_str!("../include/bada.h"), bada_compat::header("bada"), "run the header example again");
}

// ---------------------------------------------------------------- data

const KO_WORDS: &str = "한국\t60\n한\t30\n학교\t20\nㅋㅋ\t90\n";
const EN_WORDS: &str = "the\nhello\n#! rare\ninterpolate\n";

fn work() -> Value {
    json!({"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": [
        {"id": "work", "name": "work", "layouts": ["ko-2-set", "en-qwerty"]},
        {"id": "ja", "name": "ja", "layouts": ["ja-romaji", "en-qwerty"]}]}})
}

#[test]
fn a_known_word_ranks_over_an_unknown_one() {
    let session = Session::with_data(
        work(),
        &[("ko-words", KO_WORDS.as_bytes()), ("en-words", EN_WORDS.as_bytes()), ("no-such-buffer", b"x")],
    );
    let field = session.field(Some("work"));
    assert_eq!(rows(&field.typed("gksrnr")), ["한국", "gksrnr"], "a known Korean word");
    field.flush();
    assert_eq!(rows(&field.typed("the")), ["the", "솓"], "a common English word, over Korean that is no known word");
    field.flush();
    assert_eq!(rows(&field.typed("The")), ["The", "쏟"], "whatever its case");
    field.flush();
    assert_eq!(
        rows(&field.typed("interpolate"))[0],
        "interpolate",
        "a rare English word, over Korean that is no known word"
    );
    field.flush();
    assert_eq!(rows(&field.typed("gkr"))[0], "학", "Korean in whole syllables, over letters that are no word");
    field.flush();
    assert_eq!(rows(&field.typed("zz")), ["ㅋㅋ", "zz"], "chat jamo are words too");
    field.flush();
    // Without the lists, the same keys rank by shape alone.
    let bare = Session::with(work());
    assert_eq!(rows(&bare.field(Some("work")).typed("the")), ["솓", "the"]);
}

#[test]
fn the_users_own_words_count() {
    let session = Session::with_data(work(), &[("ko-words", KO_WORDS.as_bytes()), ("en-words", EN_WORDS.as_bytes())]);
    let field = session.field(Some("work"));
    assert_eq!(rows(&field.typed("kube"))[0], "ㅏㅕㅠㄷ");
    field.flush();
    let set = session.event(json!({"type": "dictionary", "value": {"format": "bada-dictionary", "version": 1,
        "positive": [{"word": "Kube", "language": "english"}, {"word": "サーバー", "language": "japanese"}],
        "negative": [{"word": "한국", "strength": "strong"}, {"word": "the", "language": "english"}]}}));
    assert_eq!(set.dictionary.map(|d| (d.positive, d.negative)), Some((2, 2)));
    assert_eq!(rows(&field.typed("kube"))[0], "kube", "a positive word is a known word");
    field.flush();
    let gone = field.typed("gksrnr");
    assert_eq!(
        (gone.preedit.text.as_str(), gone.menu.rows.len()),
        ("gksrnr", 0),
        "a strong negative word is never offered"
    );
    field.flush();
    assert_eq!(rows(&field.typed("the")), ["솓", "the"], "a soft one is ranked far down");
    field.flush();
    assert!(session.event(json!({"type": "dictionary", "value": null})).ok);
    assert_eq!(rows(&field.typed("gksrnr"))[0], "한국", "and cleared, it is as before");
}

#[test]
fn bada_imes_own_lists_load() {
    let ko = std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/data/ko-words.tsv")).unwrap();
    let en = std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/data/en-words.txt")).unwrap();
    let ja = std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/data/ja-words.txt")).unwrap();
    let native = std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/data/ja-kana-native.txt")).unwrap();
    let session = Session::with_data(
        work(),
        &[("ko-words", &ko), ("en-words", &en), ("ja-words", &ja), ("ja-kana-native", &native)],
    );
    let field = session.field(Some("work"));
    for (keys, want) in [
        ("gksrnrdj", "한국어"),
        ("dkssudgktpdy", "안녕하세요"),
        ("hello", "hello"),
        ("interpolate", "interpolate"),
        ("rkatkgkqslek", "감사합니다"),
    ] {
        assert_eq!(rows(&field.typed(keys))[0], want, "{keys}");
        field.flush();
    }
    let field = session.field(Some("ja"));
    assert_eq!(rows(&field.typed("anata"))[0], "あなた");
    field.flush();
    assert_eq!(rows(&field.typed("hello"))[0], "hello", "a common English word, over kana that is no word");
}

fn file(name: &str) -> Vec<u8> {
    std::fs::read(format!("{}/data/{name}", env!("CARGO_MANIFEST_DIR"))).unwrap()
}

#[test]
fn japanese_converts_to_kanji() {
    let (kanji, words, native, common) =
        (file("kanji.bin"), file("ja-words.txt"), file("ja-kana-native.txt"), file("ja-common-words.txt"));
    let settings = json!({"henkan": {"profiles": [
        {"id": "ja", "name": "ja", "layouts": ["ja-romaji"]},
        {"id": "forms", "name": "forms", "layouts": ["ja-romaji"], "script_forms": true}]}});
    let session = Session::with_data(
        settings,
        &[("kanji", &kanji), ("ja-words", &words), ("ja-kana-native", &native), ("ja-common-words", &common)],
    );
    let field = session.field(Some("ja"));
    // Typing shows the top conversion; the rows carry their reading, the kana row its romaji.
    let out = field.typed("watashihagakuseidesu");
    assert_eq!(out.preedit.text, "私は学生です");
    assert_eq!(out.menu.rows[0].note.as_deref(), Some("わたしはがくせいです"));
    let kana = out.menu.rows.iter().find(|r| r.text == "わたしはがくせいです").expect("the kana is a row");
    assert_eq!(kana.note.as_deref(), Some("watashihagakuseidesu"));
    assert!(out.menu.rows.len() <= 8);
    assert_eq!(field.key("Space").commit, "私は学生です");
    // A word usually written in kana: its kana first.
    assert_eq!(rows(&field.typed("anata"))[0], "あなた");
    field.flush();
    // A known word's kana no lower than second.
    let boku = field.typed("boku");
    assert_eq!(&rows(&boku)[..2], ["僕", "ぼく"]);
    field.flush();
    // The most common words of a lone reading come under the first conversion.
    let hi = field.typed("hi");
    assert!(rows(&hi)[..4].contains(&"日") && rows(&hi)[..4].contains(&"火"), "{:?}", rows(&hi));
    field.flush();
    // A broken reading isn't converted.
    assert_eq!(rows(&field.typed("ky")), Vec::<&str>::new());
    assert_eq!(field.flush(), "ky");
    // The user's own word comes first.
    assert!(
        session
            .event(json!({"type": "dictionary", "value": {"format": "bada-dictionary", "version": 1,
        "positive": [{"word": "サーバー", "language": "japanese"}]}}))
            .ok
    );
    assert_eq!(rows(&field.typed("sa-ba-"))[0], "サーバー");
    field.flush();
    // With the forms on, katakana is entry −2 and isn't repeated among the conversions.
    let field = session.field(Some("forms"));
    let out = field.typed("ra-men");
    let kata: Vec<i32> = out.menu.rows.iter().filter(|r| r.text == "ラーメン").map(|r| r.entry).collect();
    assert_eq!(kata, [-2]);
}

#[test]
fn key_models_say_which_layout_was_meant() {
    let (models, ko, en, ja, native, kanji) = (
        file("key-models.bin"),
        file("ko-words.tsv"),
        file("en-words.txt"),
        file("ja-words.txt"),
        file("ja-kana-native.txt"),
        file("kanji.bin"),
    );
    let settings = json!({"henkan": {"profiles": [
        {"id": "ko-en", "name": "x", "layouts": ["ko-2-set", "en-qwerty"]},
        {"id": "three", "name": "x", "layouts": ["en-qwerty", "ko-2-set", "ja-romaji"]},
        {"id": "roman", "name": "x", "layouts": ["ko-bada-roman", "ja-romaji"]},
        {"id": "alone", "name": "x", "layouts": ["ko-2-set"]}]}});
    let buffers: [(&str, &[u8]); 6] = [
        ("key-models", &models),
        ("ko-words", &ko),
        ("en-words", &en),
        ("ja-words", &ja),
        ("ja-kana-native", &native),
        ("kanji", &kanji),
    ];
    let session = Session::with_data(settings, &buffers);
    let top = |profile: &str, keys: &str| {
        let field = session.field(Some(profile));
        field.typed(keys).preedit.text
    };
    // Keys that look like English show as English; keys that look like Korean, Korean.
    for (keys, want) in [
        ("the", "the"),
        ("gksrnrdj", "한국어"),
        ("dkssudgktpdy", "안녕하세요"),
        ("hello", "hello"),
        ("interpolate", "interpolate"),
        ("rkatkgkqslek", "감사합니다"),
    ] {
        assert_eq!(top("ko-en", keys), want, "{keys}");
    }
    for (keys, want) in
        [("konnichiha", "こんにちは"), ("dkssudgktpdy", "안녕하세요"), ("hello", "hello"), ("arigatou", "ありがとう")]
    {
        assert_eq!(top("three", keys), want, "{keys}");
    }
    // bada roman beside romaji, whose letters are the same keys.
    assert_eq!(top("roman", "hanguk"), "한국");
    assert_eq!(top("roman", "kareha"), "彼は");
    // A layout alone is ranked as it always was.
    assert_eq!(top("alone", "the"), "솓");
}

#[test]
fn searches_find_symbols_emoji_and_characters() {
    let (symbols, emoji, names) = (file("symbols.tsv"), file("emoji.tsv"), file("unicode-names.tsv"));
    let settings = json!({"henkan": {"profiles": [
        {"id": "sym", "name": "x", "layouts": ["uni-symbols"]},
        {"id": "emoji", "name": "x", "layouts": ["uni-emojis"]},
        {"id": "toned", "name": "x", "layouts": ["uni-emojis"],
         "options": {"uni-emojis": {"skin_tones": ["medium", "none"]}}},
        {"id": "code", "name": "x", "layouts": ["uni-unicode"]},
        {"id": "all", "name": "x", "layouts": ["uni-symbols", "uni-emojis", "uni-unicode"]}]}});
    let session = Session::with_data(settings, &[("symbols", &symbols), ("emoji", &emoji), ("unicode-names", &names)]);
    let find = |profile: &str, keys: &str| {
        let out = session.field(Some(profile)).typed(keys);
        let found: Vec<(String, String)> =
            out.menu.rows.iter().map(|r| (r.text.clone(), r.note.clone().unwrap_or_default())).collect();
        (out.preedit.text, found)
    };
    // The search as typed shows and is the first row; what it finds is under it, each with its name.
    let (shown, found) = find("sym", "usd");
    assert_eq!(shown, "usd");
    assert_eq!(found[..2], [("usd".to_string(), String::new()), ("$".to_string(), "usd".to_string())]);
    // A symbol typed alone lists the entries that name its key.
    let (_, found) = find("sym", ".");
    assert_eq!(found[0].0, ".");
    assert!(found.iter().any(|f| f.0 == "…") && found.iter().any(|f| f.0 == "。"), "{found:?}");
    // A digit no entry lists: its other forms.
    let (_, found) = find("sym", "1");
    assert_eq!(found[1], ("１".to_string(), "fullwidth".to_string()));
    assert!(found.iter().any(|f| f.0 == "①"));
    // Emoji, by a word of the name or a keyword; the tones in the order the profile sets.
    let (_, found) = find("emoji", "happy face");
    assert_eq!(found[1].0, "😀", "{found:?}");
    assert!(find("emoji", "w").1.is_empty(), "one row is no menu: a search of one character looks nothing up");
    let wave: Vec<String> = find("emoji", "waving hand").1.into_iter().map(|f| f.0).collect();
    assert_eq!(wave[1..7], ["👋", "👋🏻", "👋🏼", "👋🏽", "👋🏾", "👋🏿"]);
    let wave = find("toned", "waving hand").1;
    assert_eq!(wave[1], ("👋🏽".to_string(), "waving hand: medium skin tone".to_string()));
    assert_eq!(wave[2].0, "👋");
    // A character by its code, or by a name.
    assert_eq!(find("code", "1f600").1[1], ("😀".to_string(), "U+1F600 grinning face".to_string()));
    assert_eq!(find("code", "u+41").1[1].0, "A");
    assert_eq!(find("code", "middle dot").1[1].0, "·");
    assert_eq!(find("code", "joo").1[1].0, "㈜");
    // Picking a result commits it.
    let field = session.field(Some("sym"));
    field.typed("usd");
    assert_eq!(field.action("menu.next").preedit.text, "$");
    assert_eq!(field.key("Enter").commit, "$");
    // Three search layouts in one profile: each search once, what each finds under it.
    let (shown, found) = find("all", "euro");
    assert_eq!(shown, "euro");
    assert_eq!(found.iter().filter(|f| f.0 == "euro").count(), 1);
    assert!(found.iter().any(|f| f.0 == "€"), "{found:?}");
}

/// A copy of a stock layout under another id, with these forms.
fn with_forms(stock: &str, id: &str, forms: Value) -> Value {
    let mut layout: Value = serde_json::from_str(stock).unwrap();
    layout["id"] = json!(id);
    layout["forms"] = forms;
    layout
}

fn entries(reply: &Reply) -> Vec<(i32, &str)> {
    reply.menu.rows.iter().map(|r| (r.entry, r.text.as_str())).collect()
}

#[test]
fn a_layout_lists_as_many_forms_as_it_wants() {
    let session = Session::new(json!({"main": ["ko-2-set"]}));
    let five = json!([
        {"do": "script", "to": "hiragana"},
        {"do": "script", "to": "katakana"},
        {"do": "script", "to": "halfwidth-katakana"},
        {"do": "keys", "width": "full"},
        {"do": "keys", "width": "half"}
    ]);
    let layouts = [
        with_forms(include_str!("../layouts/ja-romaji.json"), "ja-five", five),
        with_forms(
            include_str!("../layouts/ko-2-set.json"),
            "ko-jamo",
            json!([{"do": "jamo", "keep": "initials"}, {"do": "keys", "width": "half"}, {"do": "jamo", "keep": "initials"}]),
        ),
        with_forms(include_str!("../layouts/en-qwerty.json"), "en-wide", json!([{"do": "keys", "width": "full"}])),
    ];
    for layout in &layouts {
        let added = session.event(json!({"type": "layout", "value": layout}));
        assert!(added.ok, "{:?}", added.problems);
    }
    let profile = |id: &str, layouts: Value, more: Value| {
        let mut p = json!({"id": id, "name": id, "layouts": layouts});
        p.as_object_mut().unwrap().extend(more.as_object().unwrap().clone());
        p
    };
    let on = json!({"script_forms": true});
    let set = session.event(json!({"type": "settings", "set": {"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": [
        profile("five", json!(["ja-five"]), on.clone()),
        profile("off", json!(["ja-five"]), json!({})),
        profile("ko", json!(["ko-jamo"]), on.clone()),
        profile("mixed", json!(["en-wide", "ko-jamo", "ja-five"]), on.clone()),
        profile("named", json!(["en-wide", "ko-jamo", "ja-five"]), json!({"script_forms": true, "script_forms_layout": "ja-five"})),
        profile("stock", json!(["ja-kana", "en-qwerty"]), on.clone()),
        profile("none", json!(["ko-2-set", "en-qwerty"]), on.clone()),
    ]}}}));
    assert!(set.ok, "{:?}", set.problems);

    // Five forms: the first of the file is −1, and the rows go down from there.
    let field = session.field(Some("five"));
    let out = field.typed("gakkou");
    assert_eq!(
        entries(&out),
        [(-5, "gakkou"), (-4, "ｇａｋｋｏｕ"), (-3, "ｶﾞｯｺｳ"), (-2, "ガッコウ"), (-1, "がっこう"), (0, "がっこう")]
    );
    assert_eq!(out.menu.selected, Some(5));
    assert!(
        out.menu.rows[..5].iter().all(|r| r.layout.as_deref() == Some("ja-five") && r.lang.as_deref() == Some("ja"))
    );
    // The previous entry goes all the way down, and stops there.
    let shown: Vec<String> = (0..6).map(|_| field.action("menu.prev").preedit.text).collect();
    assert_eq!(shown, ["がっこう", "ガッコウ", "ｶﾞｯｺｳ", "ｇａｋｋｏｕ", "gakkou", "gakkou"]);
    assert_eq!(field.action("menu.prev").menu.selected, Some(0), "no wrap");
    assert_eq!(field.action("menu.next").preedit.text, "ｇａｋｋｏｕ");
    assert_eq!(field.flush(), "ｇａｋｋｏｕ", "committing commits the form that shows");
    // A row is picked by its place, a form like any other.
    field.typed("ka");
    assert_eq!(field.ok(json!({"type": "pick", "row": 2})).preedit.text, "ｶ");
    assert_eq!(
        field.typed("n").preedit.text,
        "かん",
        "…but typing on doesn't fix a form: the reading goes on at entry 0"
    );
    field.flush();
    // A reading that isn't finished has its forms too: they are of the keys, whatever they spell.
    assert_eq!(entries(&field.typed("ky")), [(-5, "ky"), (-4, "ｋｙ"), (-3, "ky"), (-2, "ky"), (-1, "ky"), (0, "ky")]);
    field.flush();
    // After a fix, the forms are of the keys typed since.
    field.typed("ka");
    field.action("fix");
    let after = field.typed("ki");
    assert_eq!(entries(&after)[..2], [(-5, "ki"), (-4, "ｋｉ")]);
    assert_eq!(field.action("menu.prev").preedit.text, "かき");
    assert_eq!(field.action("menu.prev").preedit.text, "かキ");
    field.flush();
    // Off, as a profile is unless it says otherwise: no negative entry.
    let field = session.field(Some("off"));
    assert!(field.typed("gakkou").menu.rows.is_empty());
    assert_eq!(field.action("menu.prev").preedit.text, "がっこう");
    field.flush();

    // Any layout: a jamo of each syllable, and the keys as typed. Forms that say the same are both rows.
    let field = session.field(Some("ko"));
    let out = field.typed("gksrmf");
    assert_eq!(entries(&out), [(-3, "ㅎㄱ"), (-2, "gksrmf"), (-1, "ㅎㄱ"), (0, "한글")]);
    assert_eq!(field.action("menu.prev").preedit.text, "ㅎㄱ");
    assert_eq!(field.key("Space").commit, "ㅎㄱ");

    // With several layouts the forms are one layout's: the first of the profile that lists any…
    let field = session.field(Some("mixed"));
    let out = field.typed("gk");
    assert_eq!(entries(&out)[..2], [(-1, "ｇｋ"), (0, "하")]);
    assert_eq!(out.menu.rows[0].layout.as_deref(), Some("en-wide"));
    field.flush();
    // …or the one the profile names.
    let field = session.field(Some("named"));
    let out = field.typed("ka");
    assert_eq!(entries(&out)[..5], [(-5, "ka"), (-4, "ｋａ"), (-3, "ｶ"), (-2, "カ"), (-1, "か")]);
    assert!(out.menu.rows[5..].iter().all(|r| r.entry >= 0));
    field.flush();
    // The stock Japanese layouts: hiragana and katakana, beside another layout too.
    let field = session.field(Some("stock"));
    let out = field.typed("t");
    assert_eq!(entries(&out)[..2], [(-2, "カ"), (-1, "か")]);
    assert_eq!(out.menu.rows[2].entry, 0);
    field.flush();
    // No layout of the profile lists any: none.
    let field = session.field(Some("none"));
    assert!(field.typed("gk").menu.rows.iter().all(|r| r.entry >= 0));
    field.flush();

    // What a form can't be: an operation that needs data, or one the format doesn't have.
    for forms in [
        json!([{"do": "lookup", "data": "hanja"}]),
        json!([{"do": "script", "to": "hiragana", "entry": -1}]),
        json!([{"do": "script", "to": "cyrillic"}]),
        json!([{"to": "hiragana"}]),
    ] {
        let refused = session.event(json!({"type": "layout", "value": with_forms(include_str!("../layouts/ja-romaji.json"), "ja-wrong", forms.clone())}));
        assert!(!refused.ok, "{forms}");
    }
    // `bada-layout/2` listed script forms in `convert`: that isn't the format's any more.
    let mut old: Value = serde_json::from_str(include_str!("../layouts/ja-romaji.json")).unwrap();
    old["id"] = json!("ja-old");
    old["convert"] = json!([{"with": "script", "forms": [{"entry": -1, "to": "hiragana"}]}]);
    assert!(!session.event(json!({"type": "layout", "value": old})).ok);
}

#[test]
fn forms_come_from_the_keys_and_the_file_alone() {
    // The same keys in a bare session, and in one with the dictionaries and the user's own words.
    let settings =
        json!({"henkan": {"profiles": [{"id": "ja", "name": "ja", "layouts": ["ja-romaji"], "script_forms": true}]}});
    let bare = Session::with(settings.clone());
    let (kanji, words, native, common) =
        (file("kanji.bin"), file("ja-words.txt"), file("ja-kana-native.txt"), file("ja-common-words.txt"));
    let full = Session::with_data(
        settings,
        &[("kanji", &kanji), ("ja-words", &words), ("ja-kana-native", &native), ("ja-common-words", &common)],
    );
    assert!(
        full.event(json!({"type": "dictionary", "value": {"format": "bada-dictionary", "version": 1,
            "positive": [{"word": "サーバー", "language": "japanese", "count": 99}, {"word": "らーめん", "language": "japanese"}],
            "negative": [{"word": "ラーメン", "language": "japanese", "strength": "strong"}, {"word": "がっこう", "language": "japanese", "strength": "strong"}]}}))
            .ok
    );
    for text in ["ra-men", "sa-ba-", "gakkou", "kanji", "watashiha", "ky", "nihon1"] {
        let (a, b) = (bare.field(Some("ja")), full.field(Some("ja")));
        let (mut plain, mut rich) = (Reply::default(), Reply::default());
        for (i, c) in text.chars().enumerate() {
            let (code, _) = key_for(c);
            (plain, rich) = (a.key(&code), b.key(&code));
            let negative = |r: &Reply| {
                r.menu.rows.iter().filter(|r| r.entry < 0).map(|r| (r.entry, r.text.clone())).collect::<Vec<_>>()
            };
            assert_eq!(negative(&plain), negative(&rich), "{text}, after {} keys", i + 1);
            assert_eq!(negative(&plain).len(), 2);
        }
        assert!(rich.menu.rows.len() >= plain.menu.rows.len(), "the dictionaries only add rows from 0 up");
        // Twice the previous entry is katakana, whatever is ranked above entry 0.
        b.action("menu.prev");
        let kata = b.action("menu.prev").preedit.text;
        a.action("menu.prev");
        assert_eq!(kata, a.action("menu.prev").preedit.text, "{text}");
        // A conversion that a form already writes isn't listed again; a word of the user's own is.
        let again = rich.menu.rows.iter().filter(|r| r.entry >= 0 && r.text == kata).count();
        if rich.menu.rows.iter().any(|r| r.entry == -1 && r.text != kata) {
            assert_eq!(again, usize::from(text == "sa-ba-"), "{text}: {:?}", entries(&rich));
        }
        a.flush();
        b.flush();
    }
}

#[test]
fn half_width_takes_kana_that_shows_or_is_selected() {
    let (kanji, words, native, common) =
        (file("kanji.bin"), file("ja-words.txt"), file("ja-kana-native.txt"), file("ja-common-words.txt"));
    let settings = json!({"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": [
        {"id": "ja", "name": "ja", "layouts": ["ja-romaji"], "script_forms": true},
        {"id": "plain", "name": "plain", "layouts": ["ja-romaji"]},
        {"id": "kana", "name": "kana", "layouts": ["ja-kana", "en-qwerty"]}]}});
    let session = Session::with_data(
        settings,
        &[("kanji", &kanji), ("ja-words", &words), ("ja-kana-native", &native), ("ja-common-words", &common)],
    );
    let field = session.field(Some("ja"));
    // Hiragana that shows: half-width katakana, as Japanese input methods make it.
    assert_eq!(field.typed("kana").preedit.text, "かな");
    let half = field.action("width.half");
    assert_eq!((half.preedit.text.as_str(), half.staged.as_str()), ("ｶﾅ", "ｶﾅ"));
    assert!(half.menu.rows.is_empty());
    assert_eq!(field.typed("ka").preedit.text, "ｶﾅか", "it stays staged, and typing goes on after it");
    assert_eq!(field.action("width.half").preedit.text, "ｶﾅｶ");
    assert_eq!(field.action("width.full").preedit.text, "カナカ", "full-width puts it back, as katakana");
    assert_eq!(field.flush(), "カナカ");
    // Katakana that shows, as the top entry or as a form: voiced kana take their mark, ー and small kana too.
    let plain = session.field(Some("plain"));
    assert_eq!(plain.typed("pa-thi-").preedit.text, "パーティー");
    assert_eq!(plain.action("width.half").preedit.text, "ﾊﾟｰﾃｨｰ");
    assert_eq!(plain.flush(), "ﾊﾟｰﾃｨｰ");
    field.typed("gakkou");
    field.action("menu.prev");
    assert_eq!(field.action("menu.prev").preedit.text, "ガッコウ");
    assert_eq!(field.action("width.half").preedit.text, "ｶﾞｯｺｳ");
    assert_eq!(field.flush(), "ｶﾞｯｺｳ");
    // The hiragana form, then half-width.
    field.typed("gakkou");
    assert_eq!(field.action("menu.prev").preedit.text, "がっこう");
    assert_eq!(field.action("width.half").preedit.text, "ｶﾞｯｺｳ");
    field.flush();
    // JIS kana beside QWERTY.
    let field = session.field(Some("kana"));
    assert_eq!(field.typed("t").preedit.text, "か");
    assert_eq!(field.action("width.half").preedit.text, "ｶ");
    field.flush();
    // Nothing staged: the selection, in henkan and in muhenkan. Kanji have no other width.
    for mode in [Some("ja"), None] {
        let field = session.field(mode);
        for (selected, made) in
            [("かな", "ｶﾅ"), ("ガッコウ", "ｶﾞｯｺｳ"), ("ぱーてぃー、「ヴ」・。", "ﾊﾟｰﾃｨｰ､｢ｳﾞ｣･｡"), ("漢字とかな", "漢字ﾄｶﾅ")]
        {
            assert_eq!(field.action("width.half").ask, Some(Ask::Selection("half".into())));
            let out = field.ok(json!({"type": "text", "selection": selected}));
            assert_eq!((out.commit.as_str(), out.replace), (made, Replace::Selection), "{mode:?}");
        }
        assert_eq!(field.action("width.full").ask, Some(Ask::Selection("full".into())));
        assert_eq!(field.ok(json!({"type": "text", "selection": "ｶﾞｯｺｳ"})).commit, "ガッコウ");
    }
}
