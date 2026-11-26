//! The settings page's host (`settings/README.md`): what it says is wrong with a file, what it
//! does to the folder, and the same over HTTP as the page asks it.

use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use bada_cli::check;
use bada_cli::folder::{Folder, HISTORY};
use bada_cli::server::{Options, Server};
use bada_compat::Level;
use serde_json::{Value, json};

const ROOT: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/..");

/// A folder of the test's own, removed when it goes.
struct Temp(PathBuf);

impl Temp {
    fn new(name: &str) -> Temp {
        static COUNT: AtomicU32 = AtomicU32::new(0);
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let unique = format!("bada-cli-{name}-{}-{nanos}-{}", std::process::id(), COUNT.fetch_add(1, Ordering::SeqCst));
        let dir = std::env::temp_dir().join(unique);
        std::fs::create_dir_all(&dir).unwrap();
        Temp(dir)
    }
}

impl Drop for Temp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn read(path: &str) -> String {
    std::fs::read_to_string(Path::new(ROOT).join(path)).unwrap()
}

/// The schema's example settings file, with a change.
fn example(change: impl FnOnce(&mut Value)) -> String {
    let mut file: Value = serde_json::from_str(&read("docs/schemas/examples/settings.json")).unwrap();
    change(&mut file);
    serde_json::to_string_pretty(&file).unwrap()
}

/// A layout of the user's own: Korean 2-set under another id.
fn my_layout(id: &str) -> String {
    let mut layout: Value = serde_json::from_str(&read("engine/layouts/ko-2-set.json")).unwrap();
    layout["id"] = json!(id);
    layout.to_string()
}

fn said(problems: &[bada_compat::Problem]) -> Vec<String> {
    problems
        .iter()
        .map(|p| format!("{} {} {}", if p.level == Level::Error { "error" } else { "warning" }, p.path, p.code))
        .collect()
}

#[test]
fn settings_are_checked_as_one_file() {
    let temp = Temp::new("check");
    let folder = Folder::at(&temp.0);
    let checked = check::settings(&folder, &example(|_| {}));
    assert!(checked.ok() && checked.problems.is_empty(), "{:?}", checked.problems);
    assert_eq!(checked.layouts.len(), 11, "the stock eleven");
    assert!(
        check::settings(&folder, r#"{"format": "bada-settings/2"}"#).ok(),
        "a file with only its format is a new install"
    );

    let of = |text: &str| said(&check::settings(&folder, text).problems);
    assert_eq!(of("{"), ["error  bad-json"]);
    assert_eq!(of("[]"), ["error  bad-value"]);
    assert_eq!(of(&example(|f| f["format"] = json!("bada-settings/1"))), ["error format bad-format"]);
    assert_eq!(of(&example(|f| f["extra"] = json!(1))), ["error extra unknown-field"]);
    // The engine's part, with its paths into the whole file.
    assert_eq!(
        of(&example(|f| f["engine"]["muhenkan"]["layout"] = json!("ko-2-set"))),
        ["error engine.muhenkan.layout not-muhenkan"]
    );
    assert_eq!(
        of(&example(|f| f["engine"]["henkan"]["profiles"][0]["layouts"] = json!(["ja-nothing"]))),
        ["error engine.henkan.profiles[0].layouts[0] unknown-layout"]
    );
    // The keys, and the harness's part.
    assert_eq!(
        of(&example(|f| f["keys"]["bindings"][0]["on"] = json!("shift+ctrl+Lang1"))),
        ["error keys.bindings[0].on bad-value"]
    );
    assert_eq!(
        of(&example(|f| f["keys"]["bindings"][0]["do"] = json!(["profile:gone"]))),
        ["warning keys.bindings[0].do[0] unknown-profile"]
    );
    assert_eq!(of(&example(|f| f["harness"] = json!([]))), ["error harness bad-value"]);
    assert_eq!(
        of(&example(|f| f["harness"]["com.teammirae.bada.mac"] = json!(1))),
        ["error harness.com.teammirae.bada.mac bad-value"]
    );
    // Every part's problems show at once.
    assert_eq!(
        of(&example(|f| {
            f["engine"]["muhenkan"]["layout"] = json!("ko-2-set");
            f["keys"]["caps_lock"] = json!("toggle");
        })),
        ["error engine.muhenkan.layout not-muhenkan", "error keys.caps_lock bad-value"]
    );

    // A profile may name a layout of the folder's own, once it is there.
    let mine = example(|f| f["engine"]["henkan"]["profiles"][0]["layouts"] = json!(["ko-my-2-set"]));
    assert_eq!(of(&mine), ["error engine.henkan.profiles[0].layouts[0] unknown-layout"]);
    assert!(folder.write("layouts/ko-my-2-set.json", &my_layout("ko-my-2-set")).ok);
    let checked = check::settings(&folder, &mine);
    assert!(checked.ok(), "{:?}", checked.problems);
    assert!(
        checked.layouts.iter().any(|l| l.id == "ko-my-2-set" && !l.stock),
        "the answer lists the user's layouts too"
    );
    // …and a binding may name one of its actions.
    let action = |name: &str| example(|f| f["keys"]["bindings"][0]["do"] = json!([name]));
    assert_eq!(of(&action("ko-my-2-set.hanja")), Vec::<String>::new());
    assert_eq!(of(&action("ko-my-2-set.nothing")), ["warning keys.bindings[0].do[0] unknown-action"]);
    assert_eq!(of(&action("ko-other.hanja")), ["warning keys.bindings[0].do[0] unknown-layout"]);
}

#[test]
fn layouts_and_dictionaries_are_checked_by_the_engine() {
    let temp = Temp::new("parts");
    let folder = Folder::at(&temp.0);
    let (checked, id) = check::layout(&folder, &my_layout("ko-my-2-set"));
    assert!(checked.ok(), "{:?}", checked.problems);
    assert_eq!(id.as_deref(), Some("ko-my-2-set"));
    assert!(checked.layouts.iter().any(|l| l.id == "ko-my-2-set"), "a dry run answers as the real one would");
    assert!(!temp.0.join("layouts").exists(), "and writes nothing");
    assert_eq!(said(&check::layout(&folder, "{").0.problems), ["error  bad-json"]);
    let old = my_layout("ko-my-2-set").replace("bada-layout/3", "bada-layout/2");
    assert_eq!(said(&check::layout(&folder, &old).0.problems), ["error format bad-format"]);

    let words = read("docs/reference/dictionary-example.json");
    assert!(check::dictionary(&folder, &words).ok());
    assert!(!check::dictionary(&folder, r#"{"format": "something-else"}"#).ok());
    assert_eq!(said(&check::dictionary(&folder, "nonsense").problems), ["error  bad-json"]);
}

#[test]
fn writes_are_checked_keep_history_and_stay_in_the_folder() {
    let outer = Temp::new("folder");
    let dir = outer.0.join("bada");
    let folder = Folder::at(&dir);
    let files = |folder: &Folder| folder.files();
    assert_eq!(
        files(&folder),
        json!({"settings": null, "dictionary": null, "layouts": [], "history": []}),
        "a new install"
    );

    // The first write makes the folder; nothing was replaced, so nothing is kept.
    let first = example(|_| {});
    let out = folder.write("settings.json", &first);
    assert!(out.ok && out.problems.is_empty(), "{:?}", out.problems);
    assert_eq!(files(&folder)["settings"]["text"], json!(first));
    assert!(files(&folder)["settings"]["modified"].as_u64().unwrap() > 1_700_000_000);
    assert!(folder.history().is_empty());

    // A file with an error changes nothing, and says why.
    let refused = folder.write("settings.json", &example(|f| f["keys"]["caps_lock"] = json!("toggle")));
    assert!(!refused.ok);
    assert_eq!(said(&refused.problems), ["error keys.caps_lock bad-value"]);
    assert_eq!(files(&folder)["settings"]["text"], json!(first));
    assert!(folder.history().is_empty());
    // A warning is taken, and said.
    // The saves below happen at moments of the test's own, all before now.
    let at = |secs: u64| UNIX_EPOCH + Duration::from_secs(1_600_000_000 + secs);
    let warned = example(|f| f["keys"]["cycle"] = json!(["gone"]));
    let out = folder.write_at("settings.json", &warned, at(0));
    assert!(out.ok);
    assert_eq!(said(&out.problems), ["warning keys.cycle[0] unknown-profile"]);

    // The file that was replaced is in history/, under the moment's name; the same text again is no change.
    assert_eq!(folder.history().len(), 1);
    assert_eq!(std::fs::read_to_string(dir.join("history").join(&folder.history()[0])).unwrap(), first);
    assert_eq!(folder.history(), ["settings-20200913-122640.json"]);
    assert!(folder.write_at("settings.json", &warned, at(50)).ok);
    assert_eq!(folder.history().len(), 1);

    // The newest twenty are kept, newest first; two saves in one second have two names.
    let version = |n: usize| example(|f| f["engine"]["henkan"]["profiles"][0]["name"] = json!(format!("version {n}")));
    for n in 0..HISTORY + 5 {
        assert!(folder.write_at("settings.json", &version(n), at(100 + n as u64 / 2)).ok, "{n}");
    }
    let history = folder.history();
    assert_eq!(history.len(), HISTORY);
    assert!(history.windows(2).all(|pair| pair[0] > pair[1]), "newest first: {history:?}");
    let kept = |name: &str| std::fs::read_to_string(dir.join("history").join(name)).unwrap();
    assert_eq!(kept(&history[0]), version(HISTORY + 3), "the newest is the file the last save replaced");
    assert_eq!(kept(&history[HISTORY - 1]), version(4), "the oldest five are gone");
    assert_eq!(files(&folder)["history"].as_array().unwrap().len(), HISTORY);
    assert_eq!(files(&folder)["history"][0]["name"], json!(history[0]));

    // Undo puts a history file back, and the file it replaces goes into the history in its turn.
    let back = folder.undo(&history[3]);
    assert!(back.ok, "{:?}", back.problems);
    assert_eq!(files(&folder)["settings"]["text"], json!(kept(&history[3])));
    assert_eq!(kept(&folder.history()[0]), version(HISTORY + 4));
    for bad in
        ["settings.json", "../settings.json", "settings-1-1.json/../../settings.json", "settings-20990101-000000.json"]
    {
        assert!(!folder.undo(bad).ok, "{bad}");
    }

    // A layout's file is named by its id, and only a slug is a name.
    let mine = my_layout("ko-my-2-set");
    let out = folder.write("layouts/ko-my-2-set.json", &mine);
    assert!(out.ok, "{:?}", out.problems);
    assert_eq!(said(&out.problems), ["warning  no-model"], "the engine's warning comes with it");
    assert_eq!(files(&folder)["layouts"][0]["id"], json!("ko-my-2-set"));
    assert_eq!(files(&folder)["layouts"][0]["text"], json!(mine));
    let other = folder.write("layouts/ko-other.json", &mine);
    assert!(!other.ok);
    assert!(said(&other.problems).contains(&"error id bad-value".to_string()), "{:?}", other.problems);
    assert!(!folder.write("layouts/ko-broken.json", "{").ok);
    assert_eq!(files(&folder)["layouts"].as_array().unwrap().len(), 1);

    // The dictionary.
    let words = read("docs/reference/dictionary-example.json");
    assert!(folder.write("dictionary.json", &words).ok);
    assert!(!folder.write("dictionary.json", r#"{"format": "something-else"}"#).ok);
    assert_eq!(files(&folder)["dictionary"]["text"], json!(words));

    // Nothing outside the folder, and nothing that isn't one of its files.
    for bad in [
        "../x",
        "../x.json",
        "layouts/../../x.json",
        "/tmp/x.json",
        "state.json",
        "history/settings-1-1.json",
        "layouts/x.json",
        "",
    ] {
        let out = folder.write(bad, &first);
        assert!(!out.ok, "{bad}");
        assert_eq!(said(&out.problems), ["error name bad-value"], "{bad}");
        assert!(!folder.remove(bad).ok, "{bad}");
    }
    assert!(!folder.remove("settings.json").ok, "the settings file isn't removed");
    let outside: Vec<String> =
        std::fs::read_dir(&outer.0).unwrap().map(|e| e.unwrap().file_name().into_string().unwrap()).collect();
    assert_eq!(outside, ["bada"], "nothing was written beside the folder");

    // Removing; and no temporary file is left anywhere.
    assert!(folder.remove("layouts/ko-my-2-set.json").ok && folder.remove("dictionary.json").ok);
    assert!(folder.remove("dictionary.json").ok, "a file that is gone is removed already");
    assert_eq!(files(&folder)["layouts"], json!([]));
    assert_eq!(files(&folder)["dictionary"], Value::Null);
    fn left(dir: &Path, out: &mut Vec<String>) {
        for entry in std::fs::read_dir(dir).unwrap().flatten() {
            if entry.path().is_dir() {
                left(&entry.path(), out);
            } else {
                out.push(entry.file_name().into_string().unwrap());
            }
        }
    }
    let mut names = Vec::new();
    left(&dir, &mut names);
    assert!(names.iter().all(|name| !name.ends_with(".tmp")), "{names:?}");
}

/// One request, and its answer: the status, the head and the body.
fn http(port: u16, request: &str) -> (u16, String, String) {
    let mut stream = TcpStream::connect(("127.0.0.1", port)).unwrap();
    stream.set_read_timeout(Some(Duration::from_secs(20))).unwrap();
    stream.write_all(request.as_bytes()).unwrap();
    let mut answer = Vec::new();
    stream.read_to_end(&mut answer).unwrap();
    let answer = String::from_utf8(answer).unwrap();
    let (head, body) = answer.split_once("\r\n\r\n").unwrap_or((&answer, ""));
    let status = head.split(' ').nth(1).and_then(|s| s.parse().ok()).unwrap_or(0);
    (status, head.to_string(), body.to_string())
}

struct Page {
    port: u16,
    token: String,
}

impl Page {
    fn request(&self, method: &str, path: &str, headers: &str, body: &str) -> (u16, String, String) {
        let length = if method == "POST" { format!("Content-Length: {}\r\n", body.len()) } else { String::new() };
        let host = if headers.contains("Host:") { String::new() } else { format!("Host: 127.0.0.1:{}\r\n", self.port) };
        http(self.port, &format!("{method} {path} HTTP/1.1\r\n{host}{headers}{length}\r\n{body}"))
    }

    /// A call of the host's interface, as the page makes it: with the token.
    fn call(&self, method: &str, path: &str, body: Value) -> (u16, Value) {
        let headers = format!("X-Bada-Token: {}\r\nOrigin: http://127.0.0.1:{}\r\n", self.token, self.port);
        let body = if method == "POST" { body.to_string() } else { String::new() };
        let (status, _, answer) = self.request(method, path, &headers, &body);
        (status, serde_json::from_str(&answer).unwrap_or(Value::Null))
    }
}

#[test]
fn the_page_is_served_and_asks_through_the_interface() {
    let temp = Temp::new("server");
    let (site, dir) = (temp.0.join("page"), temp.0.join("bada"));
    std::fs::create_dir_all(site.join("lib")).unwrap();
    std::fs::write(site.join("index.html"), "<!DOCTYPE html><p>the page</p>").unwrap();
    std::fs::write(site.join("lib/page.js"), "export const page = 1;").unwrap();
    std::fs::write(temp.0.join("secret.txt"), "not the page's").unwrap();

    let server = Server::bind(Options::new(Folder::at(&dir), site)).unwrap();
    let page = Page { port: server.port(), token: server.token().to_string() };
    assert_eq!(server.url(), format!("http://127.0.0.1:{}/?t={}", page.port, page.token));
    assert_eq!(page.token.len(), 32);
    let running = std::thread::spawn(move || server.run());

    // The page's files: no token needed, and nothing outside its folder.
    let (status, head, body) = page.request("GET", &format!("/?t={}", page.token), "", "");
    assert_eq!((status, body.as_str()), (200, "<!DOCTYPE html><p>the page</p>"));
    assert!(head.contains("Content-Type: text/html") && head.contains("Cache-Control: no-store"), "{head}");
    let (status, head, body) = page.request("GET", "/lib/page.js", "", "");
    assert_eq!((status, body.as_str()), (200, "export const page = 1;"));
    assert!(head.contains("Content-Type: text/javascript"), "{head}");
    for missing in ["/../secret.txt", "/lib/../../secret.txt", "/nothing.js", "/lib"] {
        assert_eq!(page.request("GET", missing, "", "").0, 404, "{missing}");
    }
    assert_eq!(page.request("POST", "/index.html", "", "").0, 405);
    assert_eq!(http(page.port, "nonsense\r\n\r\n").0, 400);

    // The token, and whose request it is.
    assert_eq!(page.request("GET", "/api/info", "", "").0, 403, "no token");
    assert_eq!(page.request("GET", "/api/info", "X-Bada-Token: 00000000000000000000000000000000\r\n", "").0, 403);
    let token = format!("X-Bada-Token: {}\r\n", page.token);
    assert_eq!(page.request("GET", "/api/info", &token, "").0, 200);
    assert_eq!(page.request("GET", "/api/info", &format!("{token}Host: localhost:{}\r\n", page.port), "").0, 200);
    assert_eq!(page.request("GET", "/api/info", &format!("{token}Host: evil.example:{}\r\n", page.port), "").0, 403);
    assert_eq!(page.request("GET", "/", "Host: evil.example\r\n", "").0, 403, "the page's files too");
    assert_eq!(page.request("GET", "/api/info", &format!("{token}Origin: http://evil.example\r\n"), "").0, 403);
    assert_eq!(page.request("GET", "/api/info", &format!("{token}Origin: null\r\n"), "").0, 403);
    assert_eq!(
        page.request("POST", "/api/alive", &format!("{token}Origin: http://localhost:{}\r\n", page.port), "").0,
        200
    );

    // What it is, and what the engine has.
    let (status, info) = page.call("GET", "/api/info", Value::Null);
    assert_eq!(status, 200);
    assert_eq!(info["platform"], json!(bada_cli::platform()));
    assert_eq!(info["harness"], json!(bada_cli::harness_id()));
    assert_eq!(info["version"], json!(env!("CARGO_PKG_VERSION")));
    assert_eq!(info["folder"], json!(dir.display().to_string()));
    assert_eq!(info["embedded"], json!(false));
    let (_, describe) = page.call("GET", "/api/describe", Value::Null);
    assert_eq!(describe["abi"]["name"], json!("bada-compatible"));
    assert_eq!(describe["layouts"].as_array().unwrap().len(), 11);
    let (_, stock) = page.call("GET", "/api/stock", Value::Null);
    assert_eq!(stock.as_array().unwrap().len(), 11);
    assert!(stock[0]["text"].as_str().unwrap().contains("bada-layout/3"));
    let (status, state) = page.call("GET", "/api/state", Value::Null);
    assert!(status == 200 && (state.is_null() || state.is_object()));

    // A new install, a check, a write.
    let (_, files) = page.call("GET", "/api/files", Value::Null);
    assert_eq!(files, json!({"settings": null, "dictionary": null, "layouts": [], "history": []}));
    let good = example(|_| {});
    let (status, checked) = page.call("POST", "/api/check", json!({"settings": good}));
    assert_eq!((status, &checked["ok"], &checked["problems"]), (200, &json!(true), &json!([])));
    assert_eq!(checked["layouts"].as_array().unwrap().len(), 11);
    let bad = example(|f| f["keys"]["caps_lock"] = json!("toggle"));
    let (_, checked) = page.call("POST", "/api/check", json!({"settings": bad}));
    assert_eq!(checked["ok"], json!(false));
    assert_eq!(checked["problems"][0]["path"], json!("keys.caps_lock"));
    assert_eq!(checked["problems"][0]["level"], json!("error"));
    assert!(!dir.exists(), "a check writes nothing");
    let (_, checked) = page.call("POST", "/api/check", json!({"layout": my_layout("ko-my-2-set")}));
    assert_eq!(checked["ok"], json!(true));
    let (_, checked) = page.call("POST", "/api/check", json!({"dictionary": "{"}));
    assert_eq!(checked["problems"][0]["code"], json!("bad-json"));
    assert_eq!(page.call("POST", "/api/check", json!({"settings": good, "layout": "{}"})).0, 400);
    assert_eq!(page.call("POST", "/api/check", json!({})).0, 400);
    assert_eq!(page.request("POST", "/api/check", &token, "not json").0, 400);

    let (status, wrote) = page.call("POST", "/api/write", json!({"name": "settings.json", "text": good}));
    assert_eq!((status, &wrote["ok"]), (200, &json!(true)));
    assert_eq!(wrote["files"]["settings"]["text"], json!(good));
    let (_, refused) = page.call("POST", "/api/write", json!({"name": "settings.json", "text": bad}));
    assert_eq!((&refused["ok"], &refused["files"]["settings"]["text"]), (&json!(false), &json!(good)));
    let (_, outside) = page.call("POST", "/api/write", json!({"name": "../x.json", "text": good}));
    assert_eq!((&outside["ok"], &outside["problems"][0]["path"]), (&json!(false), &json!("name")));
    assert_eq!(page.call("POST", "/api/write", json!({"name": "settings.json"})).0, 400);
    let changed = example(|f| f["keys"]["caps_rule"] = json!("upper_snake"));
    let (_, wrote) = page.call("POST", "/api/write", json!({"name": "settings.json", "text": changed}));
    assert_eq!(wrote["files"]["history"].as_array().unwrap().len(), 1);
    let (_, files) = page.call("GET", "/api/files", Value::Null);
    assert_eq!(files, wrote["files"]);
    assert_eq!(std::fs::read_to_string(dir.join("settings.json")).unwrap(), changed);

    // Undo, a layout, removing.
    let name = files["history"][0]["name"].as_str().unwrap();
    let (_, back) = page.call("POST", "/api/undo", json!({"name": name}));
    assert_eq!((&back["ok"], &back["files"]["settings"]["text"]), (&json!(true), &json!(good)));
    assert_eq!(page.call("POST", "/api/undo", json!({"name": "../settings.json"})).1["ok"], json!(false));
    let layout = json!({"name": "layouts/ko-my-2-set.json", "text": my_layout("ko-my-2-set")});
    let (_, wrote) = page.call("POST", "/api/write", layout);
    assert_eq!((&wrote["ok"], &wrote["files"]["layouts"][0]["id"]), (&json!(true), &json!("ko-my-2-set")));
    let (_, removed) = page.call("POST", "/api/remove", json!({"name": "layouts/ko-my-2-set.json"}));
    assert_eq!((&removed["ok"], &removed["files"]["layouts"]), (&json!(true), &json!([])));
    assert_eq!(page.call("POST", "/api/remove", json!({"name": "settings.json"})).1["ok"], json!(false));

    // The wrong method, a call the host doesn't have.
    assert_eq!(page.call("GET", "/api/write", Value::Null).0, 405);
    assert_eq!(page.call("POST", "/api/info", json!({})).0, 405);
    assert_eq!(page.call("GET", "/api/nothing", Value::Null).0, 404);

    // Goodbye stops it.
    assert_eq!(page.call("POST", "/api/bye", json!({})), (200, json!({"ok": true})));
    let started = Instant::now();
    running.join().unwrap().unwrap();
    assert!(started.elapsed() < Duration::from_secs(5));
    assert!(TcpStream::connect(("127.0.0.1", page.port)).is_err(), "nothing listens any more");
}

#[test]
fn a_page_that_never_comes_or_goes_silent_stops_the_host() {
    let temp = Temp::new("idle");
    std::fs::write(temp.0.join("index.html"), "page").unwrap();
    let mut options = Options::new(Folder::at(temp.0.join("bada")), temp.0.clone());
    options.first = Duration::from_millis(300);
    options.idle = Duration::from_millis(600);
    let server = Server::bind(options).unwrap();
    let page = Page { port: server.port(), token: server.token().to_string() };
    let started = Instant::now();
    let running = std::thread::spawn(move || server.run());
    // Heard from in time: it stays for as long as the page may be silent.
    std::thread::sleep(Duration::from_millis(100));
    assert_eq!(page.call("POST", "/api/alive", json!({})).0, 200);
    std::thread::sleep(Duration::from_millis(350));
    assert_eq!(page.call("POST", "/api/alive", json!({})).0, 200, "past the first wait, and still there");
    running.join().unwrap().unwrap();
    let ran = started.elapsed();
    assert!(ran >= Duration::from_millis(1000) && ran < Duration::from_secs(10), "{ran:?}");

    // Never heard from at all: gone after the first wait.
    let mut options = Options::new(Folder::at(temp.0.join("bada")), temp.0.clone());
    options.first = Duration::from_millis(200);
    let server = Server::bind(options).unwrap();
    let started = Instant::now();
    server.run().unwrap();
    assert!(started.elapsed() < Duration::from_secs(5));
}
