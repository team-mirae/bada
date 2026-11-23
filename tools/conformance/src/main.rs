//! Loads any bada-compatible v2 engine and replays transcripts on it.
//!
//!     bada-conformance <library> <prefix> [--data name=path]... <transcript.jsonl>...
//!
//! A transcript is JSON Lines (README.md): events, and the replies expected of them. It knows
//! nothing of the engine but the standard's eight functions, found under `<prefix>_`.

use std::collections::HashMap;
use std::ffi::{CStr, CString, c_char, c_void};
use std::process::ExitCode;

use libloading::{Library, Symbol};
use serde_json::Value;

#[repr(C)]
struct PBuf {
    name: *const c_char,
    ptr: *const u8,
    len: usize,
    flags: u32,
}

/// The eight functions (the standard, §3), with the engine's own types opaque.
struct Abi<'lib> {
    describe: Symbol<'lib, unsafe extern "C" fn() -> *const c_char>,
    data_new: Symbol<'lib, unsafe extern "C" fn(*const PBuf, usize) -> *const c_void>,
    data_free: Symbol<'lib, unsafe extern "C" fn(*const c_void)>,
    session_new: Symbol<'lib, unsafe extern "C" fn(*const c_void, *const c_char) -> *mut c_void>,
    session_free: Symbol<'lib, unsafe extern "C" fn(*mut c_void)>,
    new: Symbol<'lib, unsafe extern "C" fn(*mut c_void) -> *mut c_void>,
    free: Symbol<'lib, unsafe extern "C" fn(*mut c_void)>,
    send: Symbol<'lib, unsafe extern "C" fn(*mut c_void, *mut c_void, *const c_char) -> *const c_char>,
}

impl<'lib> Abi<'lib> {
    fn load(lib: &'lib Library, prefix: &str) -> Result<Abi<'lib>, String> {
        macro_rules! symbol {
            ($name:literal) => {{
                let name = format!("{prefix}_{}", $name);
                // SAFETY: the standard gives each function its signature.
                unsafe { lib.get(name.as_bytes()) }.map_err(|e| format!("{name}: {e}"))?
            }};
        }
        Ok(Abi {
            describe: symbol!("describe"),
            data_new: symbol!("data_new"),
            data_free: symbol!("data_free"),
            session_new: symbol!("session_new"),
            session_free: symbol!("session_free"),
            new: symbol!("new"),
            free: symbol!("free"),
            send: symbol!("send"),
        })
    }
}

fn read(ptr: *const c_char) -> Result<Value, String> {
    if ptr.is_null() {
        return Err("the reply was NULL".into());
    }
    // SAFETY: a reply is a NUL-terminated string, valid until the next call.
    let text = unsafe { CStr::from_ptr(ptr) }.to_str().map_err(|_| "the reply isn't UTF-8".to_string())?;
    serde_json::from_str(text).map_err(|e| format!("the reply isn't JSON ({e}): {text}"))
}

/// Where `got` differs from what is expected of it. An expected object names only the fields that
/// matter; an expected array is the whole array.
fn differs(expect: &Value, got: &Value, path: &str) -> Option<String> {
    match (expect, got) {
        (Value::Object(e), Value::Object(g)) => e.iter().find_map(|(k, v)| {
            let path = if path.is_empty() { k.clone() } else { format!("{path}.{k}") };
            match g.get(k) {
                Some(have) => differs(v, have, &path),
                None if v.is_null() => None,
                None => Some(format!("{path}: missing, expected {v}")),
            }
        }),
        (Value::Array(e), Value::Array(g)) if e.len() == g.len() => {
            e.iter().zip(g).enumerate().find_map(|(i, (v, have))| differs(v, have, &format!("{path}[{i}]")))
        }
        (Value::Array(e), Value::Array(g)) => Some(format!("{path}: {} items, expected {}", g.len(), e.len())),
        _ if expect == got => None,
        _ => Some(format!("{path}: {got}, expected {expect}")),
    }
}

struct Run<'a> {
    abi: &'a Abi<'a>,
    data: *const c_void,
    session: *mut c_void,
    engines: HashMap<String, *mut c_void>,
}

impl Run<'_> {
    fn open(&mut self, init: &Value) -> Result<(), String> {
        self.close();
        let init = CString::new(init.to_string()).map_err(|e| e.to_string())?;
        // SAFETY: the data is live, and the init string is valid for the call.
        self.session = unsafe { (self.abi.session_new)(self.data, init.as_ptr()) };
        if self.session.is_null() { Err("P_session_new answered NULL".into()) } else { Ok(()) }
    }

    fn close(&mut self) {
        for (_, engine) in self.engines.drain() {
            // SAFETY: from P_new, freed once.
            unsafe { (self.abi.free)(engine) };
        }
        if !self.session.is_null() {
            // SAFETY: from P_session_new, with its engines freed.
            unsafe { (self.abi.session_free)(self.session) };
            self.session = std::ptr::null_mut();
        }
    }

    /// One line of a transcript.
    fn step(&mut self, line: &Value) -> Result<(), String> {
        if let Some(init) = line.get("init") {
            return self.open(init);
        }
        if let Some(expect) = line.get("describe") {
            // SAFETY: P_describe needs nothing.
            let got = read(unsafe { (self.abi.describe)() })?;
            return differs(expect, &got, "").map_or(Ok(()), Err);
        }
        if let Some(name) = line.get("new").and_then(Value::as_str) {
            if self.session.is_null() {
                return Err("no session: a transcript starts with init".into());
            }
            // SAFETY: the session is live.
            let engine = unsafe { (self.abi.new)(self.session) };
            if engine.is_null() {
                return Err("P_new answered NULL".into());
            }
            self.engines.insert(name.to_string(), engine);
            return Ok(());
        }
        if let Some(name) = line.get("free").and_then(Value::as_str) {
            let engine = self.engines.remove(name).ok_or_else(|| format!("no engine named {name}"))?;
            // SAFETY: from P_new, freed once.
            unsafe { (self.abi.free)(engine) };
            return Ok(());
        }
        // An event: `send` is its JSON, or `send_raw` the very text (for what isn't JSON).
        let text = match (line.get("send"), line.get("send_raw").and_then(Value::as_str)) {
            (Some(event), _) => event.to_string(),
            (None, Some(raw)) => raw.to_string(),
            (None, None) if line.get("note").is_some() || line.get("transcript").is_some() => return Ok(()),
            (None, None) => return Err("a line has init, describe, new, free, send, send_raw or note".into()),
        };
        let engine = match line.get("to").and_then(Value::as_str) {
            Some(name) => *self.engines.get(name).ok_or_else(|| format!("no engine named {name}"))?,
            None => std::ptr::null_mut(),
        };
        if self.session.is_null() {
            return Err("no session: a transcript starts with init".into());
        }
        let text = CString::new(text).map_err(|e| e.to_string())?;
        // SAFETY: live handles, and an event string valid for the call.
        let got = read(unsafe { (self.abi.send)(self.session, engine, text.as_ptr()) })?;
        match line.get("expect") {
            Some(expect) => differs(expect, &got, "").map_or(Ok(()), |d| Err(format!("{d}\n      reply: {got}"))),
            None => Ok(()),
        }
    }
}

fn run(args: &[String]) -> Result<bool, String> {
    let usage = "usage: bada-conformance <library> <prefix> [--data name=path]... <transcript.jsonl>...";
    let (library, prefix) = (args.first().ok_or(usage)?, args.get(1).ok_or(usage)?);
    let mut buffers: Vec<(CString, Vec<u8>)> = Vec::new();
    let mut transcripts = Vec::new();
    let mut rest = args[2..].iter();
    while let Some(arg) = rest.next() {
        if arg == "--data" {
            let (name, path) = rest.next().and_then(|d| d.split_once('=')).ok_or(usage)?;
            let bytes = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
            buffers.push((CString::new(name).map_err(|e| e.to_string())?, bytes));
        } else {
            transcripts.push(arg);
        }
    }
    if transcripts.is_empty() {
        return Err(usage.into());
    }
    // SAFETY: loading a library runs its initializers; the user named it.
    let lib = unsafe { Library::new(library) }.map_err(|e| format!("{library}: {e}"))?;
    let abi = Abi::load(&lib, prefix)?;
    let bufs: Vec<PBuf> =
        buffers.iter().map(|(n, b)| PBuf { name: n.as_ptr(), ptr: b.as_ptr(), len: b.len(), flags: 0 }).collect();
    // SAFETY: the buffers are valid for the call (and beyond: they aren't lent).
    let data = unsafe { (abi.data_new)(bufs.as_ptr(), bufs.len()) };
    if data.is_null() {
        return Err("P_data_new answered NULL".into());
    }
    let mut run = Run { abi: &abi, data, session: std::ptr::null_mut(), engines: HashMap::new() };
    let mut all = true;
    for path in transcripts {
        let text = std::fs::read_to_string(path).map_err(|e| format!("{path}: {e}"))?;
        let (mut passed, mut failed) = (0, 0);
        for (n, line) in text.lines().enumerate().filter(|(_, l)| !l.trim().is_empty()) {
            let result =
                serde_json::from_str::<Value>(line).map_err(|e| format!("not JSON: {e}")).and_then(|v| run.step(&v));
            match result {
                Ok(()) => passed += 1,
                Err(why) => {
                    failed += 1;
                    println!("  FAIL {path}:{}\n      {why}", n + 1);
                }
            }
        }
        run.close();
        all &= failed == 0;
        println!("{} {path}: {passed} lines passed, {failed} failed", if failed == 0 { "ok  " } else { "FAIL" });
    }
    // SAFETY: from P_data_new, with every session freed.
    unsafe { (abi.data_free)(data) };
    Ok(all)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args) {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(why) => {
            eprintln!("{why}");
            ExitCode::from(2)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::differs;
    use serde_json::json;

    #[test]
    fn an_expected_object_names_only_what_matters() {
        let got = json!({"ok": true, "commit": "", "preedit": {"text": "AB", "caret": 2}, "menu": {"rows": [{"text": "AB", "entry": 0}]}});
        assert_eq!(differs(&json!({"preedit": {"text": "AB"}}), &got, ""), None);
        assert_eq!(differs(&json!({"menu": {"rows": [{"text": "AB"}]}}), &got, ""), None);
        assert_eq!(differs(&json!({"error": null}), &got, ""), None, "null expects the field missing or null");
        assert!(differs(&json!({"preedit": {"text": "A"}}), &got, "").unwrap().starts_with("preedit.text"));
        assert!(differs(&json!({"menu": {"rows": []}}), &got, "").unwrap().contains("1 items, expected 0"));
        assert!(differs(&json!({"staged": ""}), &got, "").unwrap().contains("missing"));
    }
}
