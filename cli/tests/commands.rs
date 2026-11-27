//! The commands of §10.2 on a settings folder of their own (no file of the user's is read).

use bada_cli::folder::Folder;
use bada_cli::{commands, layouts, typing};

fn folder(name: &str) -> Folder {
    let dir = std::env::temp_dir().join(format!("bada-cli-commands-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    Folder::at(dir)
}

#[test]
fn profiles_and_keys_are_written_checked() {
    let f = folder("profiles");
    assert!(commands::profile_add(&f, "ko", &["ko-2-set".into(), "en-qwerty".into()], Some("한국어")).ok);
    assert!(commands::profile_add(&f, "ja", &["ja-romaji".into()], None).ok);
    assert!(!commands::profile_add(&f, "ko", &["ko-2-set".into()], None).ok, "a second ko");
    assert!(!commands::profile_add(&f, "Bad Id", &["ko-2-set".into()], None).ok);
    assert!(commands::profile_move(&f, "ja", 0).ok);
    let list = commands::profile_list(&f).value;
    assert_eq!(list["profiles"][0]["id"], "ja");
    assert!(commands::keys_bind(&f, "shift+Lang1", &["menu.prev".into()], Some("henkan")).ok);
    assert!(!commands::keys_bind(&f, "Lang1", &["nope".into()], None).ok, "an action that isn't one");
    assert!(!commands::keys_bind(&f, "lang1", &["menu.next".into()], None).ok, "a key that isn't one");
    assert!(commands::keys_unbind(&f, "shift+Lang1", None).ok);
    assert!(!commands::keys_unbind(&f, "shift+Lang1", None).ok, "nothing left to unbind");
    assert!(commands::settings_set(&f, "engine.muhenkan.layout", "\"en-qwerty\"").ok);
    assert_eq!(commands::settings_get(&f, "engine.muhenkan.layout").value["value"], "en-qwerty");
    // A value the engine refuses is not written.
    assert!(!commands::settings_set(&f, "engine.muhenkan.layout", "42").ok);
    assert_eq!(commands::settings_get(&f, "engine.muhenkan.layout").value["value"], "en-qwerty");
    assert!(commands::settings_undo(&f).ok);
}

#[test]
fn stock_layouts_are_changed_only_when_asked() {
    let f = folder("layouts");
    let refused = layouts::set(&f, "en-qwerty", "name.en", "\"Mine\"", false);
    assert!(!refused.ok && refused.problems[0].code == "stock-layout" && refused.problems[0].message.contains("§7.7"));
    let done = layouts::set(&f, "en-qwerty", "name.en", "\"Mine\"", true);
    assert!(done.ok && done.problems.iter().any(|p| p.code == "modified-stock"));
    assert_eq!(layouts::list(&f).value["layouts"][0]["modified"], true);
    assert!(layouts::reset(&f, "en-qwerty").ok);
    assert!(layouts::copy(&f, "en-dvorak", "en-mine").ok);
    assert!(!layouts::remove(&f, "en-dvorak").ok, "a stock layout can't be removed");
    assert!(layouts::remove(&f, "en-mine").ok);
    assert_eq!(layouts::missing(&f, "en-qwerty").value["missing"], serde_json::json!([]));
}

#[test]
fn type_composes_on_the_engine() {
    let f = folder("type");
    assert!(commands::profile_add(&f, "ko", &["ko-2-set".into()], None).ok);
    let a = typing::run(&f, Some("ko"), "dkssud");
    assert!(a.ok, "{:?}", a.problems);
    assert_eq!(a.value["staged"], "안녕");
    assert_eq!(a.value["flushed"], "안녕");
    let direct = typing::run(&f, None, "hello!");
    assert!(direct.ok, "{:?}", direct.problems);
}

#[test]
fn the_dictionary_keeps_a_word_on_one_side() {
    let f = folder("dict");
    assert!(commands::dict_add(&f, "데이터센터", false, Some("korean")).ok);
    assert!(commands::dict_add(&f, "데이터센터", true, None).ok);
    let d = commands::dict_show(&f).value;
    assert_eq!(d["dictionary"]["positive"], serde_json::json!([]));
    assert_eq!(d["dictionary"]["negative"][0]["word"], "데이터센터");
    assert!(commands::dict_remove(&f, "데이터센터").ok);
    assert!(!commands::dict_remove(&f, "데이터센터").ok);
}
