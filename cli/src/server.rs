//! The settings page's host (`settings/README.md`): the page's files and the host's interface,
//! served on 127.0.0.1 to the one page this run was started for.
//!
//! The address carries a token made for the run, and every `/api` request must carry it. A request
//! whose `Host` or `Origin` isn't the page's own is refused, so another site open in the same
//! browser can't reach the folder through the user's machine. The server stops when the page says
//! goodbye, or hasn't been heard from for a while.

use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hasher};
use std::io::{self, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime};

use serde_json::{Value, json};

use crate::folder::{Folder, Outcome};
use crate::{check, page, state};

/// The longest body a request may carry.
const MAX_BODY: usize = 8 * 1024 * 1024;
const MAX_HEAD: usize = 64 * 1024;

pub struct Options {
    pub folder: Folder,
    /// The folder of the page's files.
    pub page: PathBuf,
    /// 0: a port of the system's choosing.
    pub port: u16,
    /// The page is in the input method's own window.
    pub embedded: bool,
    /// How long the page may stay silent before the server stops.
    pub idle: Duration,
    /// How long the first request may take to come.
    pub first: Duration,
}

impl Options {
    pub fn new(folder: Folder, page: PathBuf) -> Options {
        Options {
            folder,
            page,
            port: 0,
            embedded: false,
            idle: Duration::from_secs(60),
            first: Duration::from_secs(120),
        }
    }
}

struct Shared {
    folder: Folder,
    page: PathBuf,
    embedded: bool,
    token: String,
    port: u16,
    idle: Duration,
    /// When the server stops unless the page is heard from.
    until: Mutex<Instant>,
    stop: AtomicBool,
    /// One change to the folder at a time.
    writing: Mutex<()>,
}

pub struct Server {
    listener: TcpListener,
    shared: Arc<Shared>,
}

/// 128 bits that another program can't guess: the standard library's hashers are keyed from the
/// system's randomness, anew for each state.
fn new_token() -> String {
    let nanos = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap_or_default().as_nanos();
    (0..2u64)
        .map(|part| {
            let mut hasher = RandomState::new().build_hasher();
            hasher.write_u64(part);
            hasher.write_u128(nanos);
            hasher.write_u32(std::process::id());
            format!("{:016x}", hasher.finish())
        })
        .collect()
}

/// Compares every byte, whatever the first difference is.
fn same(a: &str, b: &str) -> bool {
    a.len() == b.len() && a.bytes().zip(b.bytes()).fold(0, |diff, (x, y)| diff | (x ^ y)) == 0
}

/// Starts a program and doesn't wait for it: the browser, or the file manager.
fn start(program: &str, args: &[&str]) -> bool {
    Command::new(program).args(args).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn().is_ok()
}

/// Opens an address in the default browser.
pub fn open_browser(url: &str) -> bool {
    if cfg!(target_os = "macos") {
        start("open", &[url])
    } else if cfg!(windows) {
        start("cmd", &["/C", "start", "", url])
    } else {
        start("xdg-open", &[url])
    }
}

/// Shows a folder in the file manager.
fn reveal(dir: &Path) -> bool {
    let _ = std::fs::create_dir_all(dir);
    let dir = dir.to_string_lossy();
    if cfg!(target_os = "macos") {
        start("open", &[&dir])
    } else if cfg!(windows) {
        start("explorer", &[&dir])
    } else {
        start("xdg-open", &[&dir])
    }
}

impl Server {
    /// Binds 127.0.0.1 and makes the run's token. Nothing is served until [`Server::run`].
    pub fn bind(options: Options) -> io::Result<Server> {
        let listener = TcpListener::bind(("127.0.0.1", options.port))?;
        let port = listener.local_addr()?.port();
        listener.set_nonblocking(true)?;
        let shared = Shared {
            folder: options.folder,
            page: options.page,
            embedded: options.embedded,
            token: new_token(),
            port,
            idle: options.idle,
            until: Mutex::new(Instant::now() + options.first),
            stop: AtomicBool::new(false),
            writing: Mutex::new(()),
        };
        Ok(Server { listener, shared: Arc::new(shared) })
    }

    pub fn port(&self) -> u16 {
        self.shared.port
    }

    pub fn token(&self) -> &str {
        &self.shared.token
    }

    /// The page's address, with the token it needs.
    pub fn url(&self) -> String {
        format!("http://127.0.0.1:{}/?t={}", self.shared.port, self.shared.token)
    }

    /// Serves until the page says goodbye or has been silent too long.
    pub fn run(self) -> io::Result<()> {
        loop {
            if self.shared.stop.load(Ordering::SeqCst) {
                return Ok(());
            }
            if Instant::now() > *self.shared.until.lock().unwrap_or_else(|p| p.into_inner()) {
                return Ok(());
            }
            match self.listener.accept() {
                Ok((stream, _)) => {
                    let shared = self.shared.clone();
                    // A browser opens connections it doesn't use at once: each has its own thread,
                    // so an idle one keeps nothing waiting.
                    std::thread::spawn(move || shared.serve(stream));
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => std::thread::sleep(Duration::from_millis(15)),
                Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                Err(_) => std::thread::sleep(Duration::from_millis(15)),
            }
        }
    }
}

struct Request {
    method: String,
    path: String,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
}

impl Request {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(n, _)| n.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
}

/// One request off a connection. Err: the status to answer with (0: nothing came, say nothing).
fn read_request(stream: &mut TcpStream) -> Result<Request, u16> {
    let mut buffer = Vec::new();
    let mut chunk = [0u8; 8192];
    let head_end = loop {
        if let Some(at) = buffer.windows(4).position(|w| w == b"\r\n\r\n") {
            break at;
        }
        if buffer.len() > MAX_HEAD {
            return Err(431);
        }
        match stream.read(&mut chunk) {
            Ok(0) => return Err(if buffer.is_empty() { 0 } else { 400 }),
            Ok(n) => buffer.extend_from_slice(&chunk[..n]),
            Err(_) => return Err(if buffer.is_empty() { 0 } else { 408 }),
        }
    };
    let head = std::str::from_utf8(&buffer[..head_end]).map_err(|_| 400u16)?;
    let mut lines = head.split("\r\n");
    let mut first = lines.next().unwrap_or("").split(' ');
    let (method, path, version) = (first.next().unwrap_or(""), first.next().unwrap_or(""), first.next().unwrap_or(""));
    if method.is_empty() || !path.starts_with('/') || !version.starts_with("HTTP/1.") {
        return Err(400);
    }
    let headers: Vec<(String, String)> = lines
        .filter_map(|line| line.split_once(':'))
        .map(|(n, v)| (n.trim().to_string(), v.trim().to_string()))
        .collect();
    let mut request = Request { method: method.to_string(), path: path.to_string(), headers, body: Vec::new() };
    if request.header("Transfer-Encoding").is_some() {
        return Err(411);
    }
    let length = match request.header("Content-Length") {
        None => 0,
        Some(length) => length.parse::<usize>().map_err(|_| 400u16)?,
    };
    if length > MAX_BODY {
        return Err(413);
    }
    let mut body = buffer[head_end + 4..].to_vec();
    while body.len() < length {
        match stream.read(&mut chunk) {
            Ok(0) | Err(_) => return Err(400),
            Ok(n) => body.extend_from_slice(&chunk[..n]),
        }
    }
    body.truncate(length);
    request.body = body;
    Ok(request)
}

fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        408 => "Request Timeout",
        411 => "Length Required",
        413 => "Content Too Large",
        431 => "Request Header Fields Too Large",
        _ => "Internal Server Error",
    }
}

fn respond(stream: &mut TcpStream, status: u16, content_type: &str, body: &[u8]) {
    let head = format!(
        "HTTP/1.1 {status} {}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nCache-Control: no-store\r\n\
         X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
        reason(status),
        body.len()
    );
    let _ = stream.write_all(head.as_bytes()).and_then(|()| stream.write_all(body)).and_then(|()| stream.flush());
    let _ = stream.shutdown(Shutdown::Write);
}

const JSON: &str = "application/json; charset=utf-8";

fn refuse(stream: &mut TcpStream, status: u16, message: &str) {
    respond(stream, status, JSON, json!({"ok": false, "error": message}).to_string().as_bytes());
}

impl Shared {
    fn serve(&self, mut stream: TcpStream) {
        // The listener doesn't block, and on some systems neither does what it accepts.
        let _ = stream.set_nonblocking(false);
        let _ = stream.set_read_timeout(Some(Duration::from_secs(10)));
        let _ = stream.set_write_timeout(Some(Duration::from_secs(10)));
        let request = match read_request(&mut stream) {
            Ok(request) => request,
            Err(0) => return,
            Err(status) => return refuse(&mut stream, status, reason(status)),
        };
        // Only the page's own address: not another name that resolves here, not another site.
        let hosts = [format!("127.0.0.1:{}", self.port), format!("localhost:{}", self.port)];
        if !request.header("Host").is_some_and(|host| hosts.iter().any(|h| h == host)) {
            return refuse(&mut stream, 403, "this isn't the page's own address");
        }
        if request.header("Origin").is_some_and(|origin| !hosts.iter().any(|h| origin == format!("http://{h}"))) {
            return refuse(&mut stream, 403, "another site can't ask");
        }
        let path = request.path.split('?').next().unwrap_or("");
        if let Some(call) = path.strip_prefix("/api/") {
            if !request.header("X-Bada-Token").is_some_and(|token| same(token, &self.token)) {
                return refuse(&mut stream, 403, "the request doesn't carry this run's token");
            }
            // The page is there.
            *self.until.lock().unwrap_or_else(|p| p.into_inner()) = Instant::now() + self.idle;
            let (status, body) = self.api(&request.method, call, &request.body);
            respond(&mut stream, status, JSON, body.as_bytes());
            if status == 200 && call == "bye" {
                self.stop.store(true, Ordering::SeqCst);
            }
            return;
        }
        if request.method != "GET" {
            return refuse(&mut stream, 405, "the page's files are read with GET");
        }
        match page::file(&self.page, path).and_then(|file| Some((std::fs::read(&file).ok()?, file))) {
            Some((bytes, file)) => respond(&mut stream, 200, page::content_type(&file), &bytes),
            None => refuse(&mut stream, 404, "the page has no such file"),
        }
    }

    fn outcome(&self, outcome: Outcome) -> String {
        json!({"ok": outcome.ok, "problems": outcome.problems, "files": self.folder.files()}).to_string()
    }

    /// One call of the host's interface: the status and the JSON to answer.
    fn api(&self, method: &str, call: &str, body: &[u8]) -> (u16, String) {
        let get = ["info", "describe", "stock", "files", "state"].contains(&call);
        let post = ["check", "write", "remove", "undo", "reveal", "alive", "bye"].contains(&call);
        if !get && !post {
            return (404, json!({"ok": false, "error": "the host has no such call"}).to_string());
        }
        if method != if get { "GET" } else { "POST" } {
            return (405, json!({"ok": false, "error": "the wrong method for this call"}).to_string());
        }
        let bad = |message: &str| (400, json!({"ok": false, "error": message}).to_string());
        let sent: Value = if post && !body.is_empty() {
            match serde_json::from_slice(body) {
                Ok(sent) => sent,
                Err(_) => return bad("the body isn't JSON"),
            }
        } else {
            Value::Null
        };
        let text = |name: &str| sent.get(name).and_then(Value::as_str);
        let answer = match call {
            "info" => json!({
                "platform": crate::platform(),
                "harness": crate::harness_id(),
                "version": crate::VERSION,
                "folder": self.folder.shown,
                "embedded": self.embedded,
            })
            .to_string(),
            "describe" => check::describe().to_string(),
            "stock" => {
                let stock: Vec<Value> =
                    bada::layout::STOCK.iter().map(|(id, text)| json!({"id": id, "text": text})).collect();
                Value::Array(stock).to_string()
            }
            "files" => self.folder.files().to_string(),
            "state" => state::read(&self.folder).to_string(),
            "check" => match (text("settings"), text("layout"), text("dictionary")) {
                (Some(settings), None, None) => check::settings(&self.folder, settings).json().to_string(),
                (None, Some(layout), None) => check::layout(&self.folder, layout).0.json().to_string(),
                (None, None, Some(dictionary)) => check::dictionary(&self.folder, dictionary).json().to_string(),
                _ => return bad("check takes one of settings, layout or dictionary, as text"),
            },
            "write" => match (text("name"), text("text")) {
                (Some(name), Some(text)) => {
                    let _one = self.writing.lock().unwrap_or_else(|p| p.into_inner());
                    self.outcome(self.folder.write(name, text))
                }
                _ => return bad("write takes a name and a text"),
            },
            "remove" | "undo" => match text("name") {
                Some(name) => {
                    let _one = self.writing.lock().unwrap_or_else(|p| p.into_inner());
                    self.outcome(if call == "remove" { self.folder.remove(name) } else { self.folder.undo(name) })
                }
                None => return bad("a name is needed"),
            },
            "reveal" => json!({"ok": reveal(&self.folder.dir)}).to_string(),
            // alive, bye
            _ => json!({"ok": true}).to_string(),
        };
        (200, answer)
    }
}
