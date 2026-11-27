//! `bada`, the command line (the standard, §10.2). See `cli/README.md` for what is written so far.

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use bada_cli::folder::Folder;
use bada_cli::server::{Options, Server, open_browser};
use bada_cli::edit::Answer;
use bada_cli::{VERSION, check, commands, layouts, page, state, typing, update};
use bada_compat::Level;
use serde_json::{Value, json};

const USAGE: &str = "\
bada, the command line (every command takes --json: one JSON object on stdout, exit code 1 on error)

  bada version                                  the command line's, the engine's, the standard's and the input method's versions
  bada update [--check]                         install the newest release (only when asked); --check only looks
  bada settings                                 open the settings page [--no-open] [--port N] [--page DIR] [--embedded]
  bada settings show | export                   the whole file
  bada settings get <path>                      one value: engine.henkan.profiles[0].layouts
  bada settings set <path> <json>               change one value (checked before it is written)
  bada settings validate | import <file> | undo
  bada profile list | remove <id>
  bada profile add <id> --layouts a,b [--name N]
  bada profile move <id> <n> | set <id> <field> <json>
  bada keys list | available
  bada keys bind <key> <action>... [--when henkan|muhenkan]
  bada keys unbind <key> [--when henkan|muhenkan]
  bada layout list | show <id> | remove <id> | reset <id> | missing <id>
  bada layout add <file> | set <id> <path> <json>   [--modify-stock]
  bada layout copy <id> <new-id> | validate <file>
  bada dict show | remove <word> | import <file>
  bada dict add <word> [--negative] [--language korean|english|japanese]
  bada type [--profile <id>] \"<keys>\"          type on the engine with no text field
  bada state                                    the input method's published state
  bada schema settings | layout | keys
";

fn usage() -> ExitCode {
    eprint!("{USAGE}");
    ExitCode::from(2)
}

fn version(json_out: bool) -> ExitCode {
    let describe: Value = serde_json::from_str(check::describe()).unwrap_or_default();
    let engine = &describe["engine"];
    let abi = &describe["abi"];
    // The installed input method says its own version in the state it publishes.
    let installed = state::read(&Folder::from_env()).pointer("/harness/version").cloned().unwrap_or(Value::Null);
    if json_out {
        let out = json!({
            "bada": VERSION,
            "engine": {"id": engine["id"], "version": engine["version"]},
            "standard": abi,
            "input_method": installed,
        });
        println!("{out}");
    } else {
        println!("bada {VERSION}");
        println!("engine {} {}", engine["id"].as_str().unwrap_or("?"), engine["version"].as_str().unwrap_or("?"));
        println!("{} {}.{}", abi["name"].as_str().unwrap_or("?"), abi["major"], abi["minor"]);
        match installed.as_str() {
            Some(running) => println!("input method {running}"),
            None => println!("input method: not running"),
        }
    }
    ExitCode::SUCCESS
}

fn validate(json_out: bool) -> ExitCode {
    let folder = Folder::from_env();
    let path = folder.dir.join("settings.json");
    let problems = match std::fs::read_to_string(&path) {
        Ok(text) => check::settings(&folder, &text).problems,
        // No file is a new install, and nothing is wrong with that.
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Vec::new(),
        Err(e) => {
            eprintln!("{}: {e}", path.display());
            return ExitCode::from(1);
        }
    };
    let ok = problems.iter().all(|p| p.level != Level::Error);
    if json_out {
        println!("{}", json!({"ok": ok, "problems": problems}));
    } else if problems.is_empty() {
        println!("{}: no problems", path.display());
    } else {
        for p in &problems {
            let level = if p.level == Level::Error { "error" } else { "warning" };
            println!("{level} {} {}: {}", if p.path.is_empty() { "(the file)" } else { &p.path }, p.code, p.message);
        }
    }
    if ok { ExitCode::SUCCESS } else { ExitCode::from(1) }
}

fn settings(args: &[String]) -> ExitCode {
    let (mut open, mut embedded, mut port, mut named) = (true, false, 0u16, None::<PathBuf>);
    let mut args = args.iter();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--no-open" => open = false,
            "--embedded" => embedded = true,
            "--port" => match args.next().and_then(|n| n.parse().ok()) {
                Some(n) => port = n,
                None => return usage(),
            },
            "--page" => match args.next() {
                Some(dir) => named = Some(PathBuf::from(dir)),
                None => return usage(),
            },
            _ => return usage(),
        }
    }
    let Some(page) = page::find(named.as_deref()) else {
        eprintln!("The settings page's files aren't there: --page names their folder (the one with index.html).");
        return ExitCode::from(1);
    };
    let mut options = Options::new(Folder::from_env(), page);
    options.port = port;
    options.embedded = embedded;
    let server = match Server::bind(options) {
        Ok(server) => server,
        Err(e) => {
            eprintln!("The page can't be served on 127.0.0.1: {e}");
            return ExitCode::from(1);
        }
    };
    // The first line: what a program that started this reads.
    let url = server.url();
    println!("{url}");
    let _ = std::io::stdout().flush();
    if open && !open_browser(&url) {
        eprintln!("The browser didn't open: open the address above.");
    }
    match server.run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(1)
        }
    }
}

/// The arguments, with the flags taken out: `--json`, and the ones a command reads.
struct Args {
    words: Vec<String>,
    json: bool,
    flags: Vec<(String, Option<String>)>,
}

/// Flags that take a value.
const VALUED: [&str; 6] = ["--when", "--layouts", "--name", "--language", "--profile", "--port"];

impl Args {
    fn parse(raw: Vec<String>) -> Args {
        let (mut words, mut json, mut flags) = (Vec::new(), false, Vec::new());
        let mut it = raw.into_iter();
        while let Some(a) = it.next() {
            if a == "--json" {
                json = true;
            } else if VALUED.contains(&a.as_str()) {
                let v = it.next();
                flags.push((a, v));
            } else if a.starts_with("--") {
                flags.push((a, None));
            } else {
                words.push(a);
            }
        }
        Args { words, json, flags }
    }

    fn flag(&self, name: &str) -> bool {
        self.flags.iter().any(|(f, _)| f == name)
    }

    fn value(&self, name: &str) -> Option<&str> {
        self.flags.iter().find(|(f, _)| f == name).and_then(|(_, v)| v.as_deref())
    }

    fn word(&self, i: usize) -> Option<&str> {
        self.words.get(i).map(String::as_str)
    }

    /// The flags a command may have; anything else is a usage error.
    fn only(&self, allowed: &[&str]) -> bool {
        self.flags.iter().all(|(f, v)| allowed.contains(&f.as_str()) && (!VALUED.contains(&f.as_str()) || v.is_some()))
    }
}

fn print(answer: Answer, json_out: bool) -> ExitCode {
    if json_out {
        println!("{}", answer.json());
    } else {
        let text = answer.render();
        if !text.is_empty() {
            if answer.ok { println!("{text}") } else { eprintln!("{text}") }
        }
    }
    if answer.ok { ExitCode::SUCCESS } else { ExitCode::from(1) }
}

fn main() -> ExitCode {
    let raw: Vec<String> = std::env::args().skip(1).collect();
    // `bada settings` (the page) has flags of its own, read as before.
    if raw.first().is_some_and(|a| a == "settings")
        && raw.get(1).is_none_or(|a| a.starts_with("--") && a != "--json")
    {
        return settings(&raw[1..]);
    }
    let a = Args::parse(raw);
    let folder = Folder::from_env();
    let w = |i| a.word(i);
    let answer = match (w(0), w(1)) {
        (Some("version"), None) if a.only(&[]) => return version(a.json),
        (Some("update"), None) if a.only(&["--check"]) => update::run(a.flag("--check")),
        (Some("state"), None) if a.only(&[]) => {
            let state = state::read(&folder);
            if a.json {
                println!("{state}");
            } else {
                println!("{}", serde_json::to_string_pretty(&state).unwrap_or_else(|_| "null".into()));
            }
            return ExitCode::SUCCESS;
        }
        (Some("schema"), Some(which)) if a.words.len() == 2 && a.only(&[]) => commands::schema(which),
        (Some("schema"), None) if a.only(&[]) => commands::schema("settings"),
        (Some("settings"), Some("validate")) if a.words.len() == 2 && a.only(&[]) => return validate(a.json),
        (Some("settings"), Some("show" | "export")) if a.words.len() == 2 && a.only(&[]) => commands::settings_show(&folder),
        (Some("settings"), Some("get")) if a.words.len() == 3 && a.only(&[]) => commands::settings_get(&folder, w(2).unwrap()),
        (Some("settings"), Some("set")) if a.words.len() == 4 && a.only(&[]) => commands::settings_set(&folder, w(2).unwrap(), w(3).unwrap()),
        (Some("settings"), Some("import")) if a.words.len() == 3 && a.only(&[]) => commands::settings_import(&folder, w(2).unwrap()),
        (Some("settings"), Some("undo")) if a.words.len() == 2 && a.only(&[]) => commands::settings_undo(&folder),
        (Some("profile"), Some("list")) if a.words.len() == 2 && a.only(&[]) => commands::profile_list(&folder),
        (Some("profile"), Some("add")) if a.words.len() == 3 && a.only(&["--layouts", "--name"]) => {
            let layouts: Vec<String> = a.value("--layouts").unwrap_or("").split(',').filter(|s| !s.is_empty()).map(String::from).collect();
            commands::profile_add(&folder, w(2).unwrap(), &layouts, a.value("--name"))
        }
        (Some("profile"), Some("remove")) if a.words.len() == 3 && a.only(&[]) => commands::profile_remove(&folder, w(2).unwrap()),
        (Some("profile"), Some("move")) if a.words.len() == 4 && a.only(&[]) => match w(3).unwrap().parse() {
            Ok(n) => commands::profile_move(&folder, w(2).unwrap(), n),
            Err(_) => return usage(),
        },
        (Some("profile"), Some("set")) if a.words.len() == 5 && a.only(&[]) => commands::profile_set(&folder, w(2).unwrap(), w(3).unwrap(), w(4).unwrap()),
        (Some("keys"), Some("list")) if a.words.len() == 2 && a.only(&[]) => commands::keys_list(&folder),
        (Some("keys"), Some("available")) if a.words.len() == 2 && a.only(&[]) => commands::keys_available(&folder),
        (Some("keys"), Some("bind")) if a.words.len() >= 4 && a.only(&["--when"]) => commands::keys_bind(&folder, w(2).unwrap(), &a.words[3..], a.value("--when")),
        (Some("keys"), Some("unbind")) if a.words.len() == 3 && a.only(&["--when"]) => commands::keys_unbind(&folder, w(2).unwrap(), a.value("--when")),
        (Some("layout"), Some("list")) if a.words.len() == 2 && a.only(&[]) => layouts::list(&folder),
        (Some("layout"), Some("show")) if a.words.len() == 3 && a.only(&[]) => layouts::show(&folder, w(2).unwrap()),
        (Some("layout"), Some("add")) if a.words.len() == 3 && a.only(&["--modify-stock"]) => layouts::add(&folder, w(2).unwrap(), a.flag("--modify-stock")),
        (Some("layout"), Some("set")) if a.words.len() == 5 && a.only(&["--modify-stock"]) => layouts::set(&folder, w(2).unwrap(), w(3).unwrap(), w(4).unwrap(), a.flag("--modify-stock")),
        (Some("layout"), Some("remove")) if a.words.len() == 3 && a.only(&[]) => layouts::remove(&folder, w(2).unwrap()),
        (Some("layout"), Some("reset")) if a.words.len() == 3 && a.only(&[]) => layouts::reset(&folder, w(2).unwrap()),
        (Some("layout"), Some("copy")) if a.words.len() == 4 && a.only(&[]) => layouts::copy(&folder, w(2).unwrap(), w(3).unwrap()),
        (Some("layout"), Some("validate")) if a.words.len() == 3 && a.only(&[]) => layouts::validate(&folder, w(2).unwrap()),
        (Some("layout"), Some("missing")) if a.words.len() == 3 && a.only(&[]) => layouts::missing(&folder, w(2).unwrap()),
        (Some("dict"), Some("show")) if a.words.len() == 2 && a.only(&[]) => commands::dict_show(&folder),
        (Some("dict"), Some("add")) if a.words.len() == 3 && a.only(&["--negative", "--language"]) => commands::dict_add(&folder, w(2).unwrap(), a.flag("--negative"), a.value("--language")),
        (Some("dict"), Some("remove")) if a.words.len() == 3 && a.only(&[]) => commands::dict_remove(&folder, w(2).unwrap()),
        (Some("dict"), Some("import")) if a.words.len() == 3 && a.only(&[]) => commands::dict_import(&folder, w(2).unwrap()),
        (Some("type"), Some(keys)) if a.words.len() == 2 && a.only(&["--profile"]) => typing::run(&folder, a.value("--profile"), keys),
        _ => return usage(),
    };
    print(answer, a.json)
}
