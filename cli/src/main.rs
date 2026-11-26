//! `bada`, the command line (the standard, §10.2). See `cli/README.md` for what is written so far.

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use bada_cli::folder::Folder;
use bada_cli::server::{Options, Server, open_browser};
use bada_cli::{VERSION, check, page, state};
use bada_compat::Level;
use serde_json::{Value, json};

const USAGE: &str = "\
bada, the command line

  bada version [--json]             the command line's, the engine's and the standard's versions
  bada settings                     open the settings page
      [--no-open]                   only print its address
      [--port N] [--page DIR]       the port to serve on, and the folder of the page's files
      [--embedded]                  the page is in the input method's own window
  bada settings validate [--json]   check settings.json, and say what is wrong with it
  bada state                        the input method's published state, or null
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

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let rest = args.get(1..).unwrap_or(&[]);
    let only_json = |rest: &[String]| match rest {
        [] => Some(false),
        [flag] if flag == "--json" => Some(true),
        _ => None,
    };
    match args.first().map(String::as_str) {
        Some("version") => only_json(rest).map_or_else(usage, version),
        Some("settings") if rest.first().is_some_and(|a| a == "validate") => {
            only_json(&rest[1..]).map_or_else(usage, validate)
        }
        Some("settings") => settings(rest),
        Some("state") if rest.is_empty() => {
            let state = state::read(&Folder::from_env());
            println!("{}", serde_json::to_string_pretty(&state).unwrap_or_else(|_| "null".into()));
            ExitCode::SUCCESS
        }
        _ => usage(),
    }
}
