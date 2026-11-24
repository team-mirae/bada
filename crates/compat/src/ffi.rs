//! The C ABI (the standard, §3). [`crate::export!`] generates the eight `extern "C"` functions for an
//! [`Engine`]; the generic code they call is here. Nothing unwinds into the harness: every call
//! into the engine is guarded.
//!
//! These take the harness's raw pointers in safe functions, as `extern "C"` entry points do: the
//! standard (§3.4) is what makes them valid.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::ffi::{CStr, CString, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, Mutex, OnceLock};

use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::event::{Change, Event, FieldEvent, SessionEvent, parse};
use crate::reply::{Error, Reply, SessionReply, code};
use crate::{ABI_NAME, Buffer, Describe, Engine, Harness, MAJOR, MINOR, Mode};

/// One named buffer, as the harness passes it to `P_data_new`.
#[repr(C)]
pub struct PBuf {
    pub name: *const c_char,
    pub ptr: *const u8,
    pub len: usize,
    pub flags: u32,
}

/// The buffer outlives the Data: the engine may keep pointers into it.
pub const P_BORROWED: u32 = 1;

/// A session as the harness holds it.
pub struct SessionHandle<E: Engine> {
    pub session: Arc<E::Session>,
    /// The event types the engine takes (`describe().events`).
    events: Vec<String>,
    /// The reply to the last event sent with no engine.
    reply: Mutex<CString>,
}

/// An engine as the harness holds it: the engine, its last reply, and what was last selected.
pub struct EngineHandle<E: Engine> {
    pub engine: E,
    session: Arc<E::Session>,
    reply: CString,
    /// The last reply to this field, for `state`.
    last: Reply,
    selected: Option<(Mode, Option<String>)>,
}

/// A NUL-terminated argument, valid for the call. NULL or invalid UTF-8: None.
///
/// # Safety
/// `ptr` is NULL or a NUL-terminated string valid for the call.
pub unsafe fn cstr<'a>(ptr: *const c_char) -> Option<&'a str> {
    if ptr.is_null() {
        return None;
    }
    // SAFETY: by the caller.
    unsafe { CStr::from_ptr(ptr) }.to_str().ok()
}

fn json(value: &impl Serialize) -> String {
    serde_json::to_string(value)
        .unwrap_or_else(|_| r#"{"ok":false,"error":{"code":"panic","message":"unwritable reply"}}"#.into())
}

/// `{"ok": false, "error": …}`: nothing changed.
fn refuse(error: Error) -> String {
    #[derive(Serialize)]
    struct Refusal {
        ok: bool,
        error: Error,
    }
    json(&Refusal { ok: false, error })
}

fn panicked() -> Error {
    Error::new(code::PANIC, "the engine panicked and has reset itself")
}

pub fn describe<E: Engine>(cell: &'static OnceLock<CString>) -> *const c_char {
    #[derive(Serialize)]
    struct Abi {
        name: &'static str,
        major: u32,
        minor: u32,
    }
    #[derive(Serialize)]
    struct Wire {
        abi: Abi,
        #[serde(flatten)]
        engine: Describe,
    }
    cell.get_or_init(|| {
        let text = catch_unwind(|| {
            json(&Wire { abi: Abi { name: ABI_NAME, major: MAJOR, minor: MINOR }, engine: E::describe() })
        })
        .unwrap_or_else(|_| "{}".into());
        CString::new(text).unwrap_or_default()
    })
    .as_ptr()
}

pub fn data_new<E: Engine>(bufs: *const PBuf, n: usize) -> *const E::Data {
    let raw: &[PBuf] = if bufs.is_null() || n == 0 {
        &[]
    } else {
        // SAFETY: the harness passes `n` buffers, valid for the call.
        unsafe { std::slice::from_raw_parts(bufs, n) }
    };
    let buffers: Vec<Buffer<'_>> = raw
        .iter()
        .filter_map(|b| {
            // SAFETY: a name valid for the call; `len` bytes at `ptr`, or nothing.
            let name = unsafe { cstr(b.name) }?;
            let bytes: &[u8] =
                if b.ptr.is_null() || b.len == 0 { &[] } else { unsafe { std::slice::from_raw_parts(b.ptr, b.len) } };
            Some(Buffer { name, bytes, borrowed: b.flags & P_BORROWED != 0 })
        })
        .collect();
    match catch_unwind(AssertUnwindSafe(|| E::data(&buffers))) {
        Ok(Some(data)) => Arc::into_raw(Arc::new(data)),
        _ => std::ptr::null(),
    }
}

pub fn data_free<E: Engine>(data: *const E::Data) {
    if !data.is_null() {
        // SAFETY: from `data_new`; every session holds its own reference.
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Arc::from_raw(data) })));
    }
}

/// What `P_session_new` is given (§3.3).
#[derive(Deserialize)]
struct Init {
    harness: Harness,
    #[serde(default)]
    layouts: Vec<Value>,
    #[serde(default)]
    settings: Option<Value>,
    #[serde(default)]
    dictionary: Option<Value>,
}

pub fn session_new<E: Engine>(data: *const E::Data, init: *const c_char) -> *mut SessionHandle<E> {
    // SAFETY: an argument string, valid for the call.
    let Some(init) = unsafe { cstr(init) }.and_then(|t| serde_json::from_str::<Init>(t).ok()) else {
        return std::ptr::null_mut();
    };
    let data = (!data.is_null()).then(|| {
        // SAFETY: from `data_new`, alive until `data_free`; the session keeps its own reference.
        unsafe {
            Arc::increment_strong_count(data);
            Arc::from_raw(data)
        }
    });
    let made = catch_unwind(AssertUnwindSafe(|| {
        let events = E::describe().events;
        let takes = |kind: &str| events.iter().any(|e| e == kind);
        let session = E::session(data, &init.harness);
        // What is wrong isn't loaded, and the session starts with the rest: the harness reads the
        // problems back with an event that changes nothing.
        if takes("layout") {
            for layout in init.layouts {
                E::layout(&session, Change::Set(layout), false);
            }
        }
        if let Some(settings) = init.settings.filter(|_| takes("settings")) {
            E::settings(&session, Change::Set(settings), false);
        }
        if let Some(dictionary) = init.dictionary.filter(|_| takes("dictionary")) {
            E::dictionary(&session, Change::Set(dictionary));
        }
        SessionHandle { session: Arc::new(session), events, reply: Mutex::new(CString::default()) }
    }));
    match made {
        Ok(handle) => Box::into_raw(Box::new(handle)),
        Err(_) => std::ptr::null_mut(),
    }
}

pub fn session_free<E: Engine>(session: *mut SessionHandle<E>) {
    if !session.is_null() {
        // SAFETY: from `session_new`.
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(session) })));
    }
}

pub fn new<E: Engine>(session: *mut SessionHandle<E>) -> *mut EngineHandle<E> {
    // SAFETY: a live handle from `session_new`.
    let Some(s) = (unsafe { session.as_ref() }) else {
        return std::ptr::null_mut();
    };
    match catch_unwind(AssertUnwindSafe(|| E::new(s.session.clone()))) {
        Ok(engine) => Box::into_raw(Box::new(EngineHandle {
            engine,
            session: s.session.clone(),
            reply: CString::default(),
            last: Reply::default(),
            selected: None,
        })),
        Err(_) => std::ptr::null_mut(),
    }
}

pub fn free<E: Engine>(engine: *mut EngineHandle<E>) {
    if !engine.is_null() {
        // SAFETY: from `new`.
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(engine) })));
    }
}

fn session_event<E: Engine>(session: &E::Session, event: SessionEvent) -> String {
    let reply: Result<SessionReply, _> = catch_unwind(AssertUnwindSafe(|| match event {
        SessionEvent::Settings { change, dry } => E::settings(session, change, dry),
        SessionEvent::Layout { change, dry } => E::layout(session, change, dry),
        SessionEvent::Dictionary { change } => E::dictionary(session, change),
    }));
    match reply {
        Ok(reply) => json(&reply),
        Err(_) => refuse(panicked()),
    }
}

fn field_event<E: Engine>(h: &mut EngineHandle<E>, event: FieldEvent) -> String {
    let echo = |reply: &mut Reply, selected: &Option<(Mode, Option<String>)>| {
        reply.ok = true;
        reply.error = None;
        reply.mode = selected.as_ref().map(|s| s.0);
        reply.profile = selected.as_ref().and_then(|s| s.1.clone());
    };
    if event == FieldEvent::State {
        // The last reply again: what shows, with nothing to do.
        let mut reply = h.last.clone();
        reply.commit.clear();
        reply.consumed = false;
        reply.types = None;
        echo(&mut reply, &h.selected);
        return json(&reply);
    }
    let engine = &mut h.engine;
    let result = catch_unwind(AssertUnwindSafe(|| match &event {
        FieldEvent::Key(key) => engine.key(key),
        FieldEvent::Action { name } => engine.action(name),
        FieldEvent::Select { mode, profile } => engine.select(*mode, profile.as_deref()),
        FieldEvent::Pick { row } => engine.pick(*row),
        FieldEvent::Text(text) => Ok(engine.text(text)),
        FieldEvent::Flush { cause } => Ok(engine.flush(*cause)),
        FieldEvent::Reset => Ok(engine.reset()),
        FieldEvent::State => unreachable!("answered above"),
    }));
    match result {
        Ok(Ok(mut reply)) => {
            if let FieldEvent::Select { mode, profile } = event {
                h.selected = Some((mode, profile));
            }
            echo(&mut reply, &h.selected);
            h.last = reply;
            json(&h.last)
        }
        Ok(Err(error)) => refuse(error),
        Err(_) => {
            // The engine may be half-changed: a new one, told what the old one was told last.
            let session = h.session.clone();
            let selected = h.selected.clone();
            let rebuilt = catch_unwind(AssertUnwindSafe(|| {
                let mut engine = E::new(session);
                if let Some((mode, profile)) = &selected {
                    let _ = engine.select(*mode, profile.as_deref());
                }
                engine
            }));
            if let Ok(engine) = rebuilt {
                // Dropping the old one may panic again; leak it rather than unwind.
                let old = std::mem::replace(&mut h.engine, engine);
                if catch_unwind(AssertUnwindSafe(|| drop(old))).is_err() {}
            }
            h.last = Reply::default();
            echo(&mut h.last, &h.selected);
            refuse(panicked())
        }
    }
}

pub fn send<E: Engine>(
    session: *mut SessionHandle<E>,
    engine: *mut EngineHandle<E>,
    event: *const c_char,
) -> *const c_char {
    // SAFETY: a live handle from `session_new`.
    let Some(s) = (unsafe { session.as_ref() }) else {
        return std::ptr::null();
    };
    // SAFETY: NULL, or a live handle from `new` used by one thread at a time (§3.4).
    let mut engine = unsafe { engine.as_mut() };
    // SAFETY: an argument string, valid for the call.
    let text = match parse(unsafe { cstr(event) }) {
        Err(error) => refuse(error),
        Ok(event) if !s.events.iter().any(|e| e == event.kind()) => {
            refuse(Error::new(code::UNKNOWN_EVENT, format!("this engine takes no {} events", event.kind())))
        }
        Ok(Event::Session(event)) => session_event::<E>(&s.session, event),
        Ok(Event::Field(event)) => match engine.as_deref_mut() {
            Some(h) => field_event(h, event),
            None => refuse(Error::new(code::NEEDS_ENGINE, "this event is about a text field, and the engine was NULL")),
        },
    };
    let text = CString::new(text).unwrap_or_default();
    match engine {
        Some(h) => {
            h.reply = text;
            h.reply.as_ptr()
        }
        None => {
            let mut reply = s.reply.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            *reply = text;
            reply.as_ptr()
        }
    }
}

/// Exports an [`Engine`](crate::Engine) as a bada-compatible v2 C ABI, in a module `ffi`.
///
/// ```ignore
/// bada_compat::export! {
///     prefix: "shout",          // every symbol is shout_<name>
///     engine: crate::Shout,
/// }
/// ```
///
/// All eight functions are exported, always: there are no optional functions in v2. What an
/// engine lacks is an event type it leaves out of `describe().events`. [`crate::header`] writes the
/// matching C header.
#[macro_export]
macro_rules! export {
    (prefix: $p:literal, engine: $engine:ty $(,)?) => {
        /// The C ABI, generated by `export!`. Rust callers (tests) may use it too.
        pub mod ffi {
            #![allow(clippy::not_unsafe_ptr_arg_deref)]
            // The engine's type is named as the caller sees it.
            #[allow(unused_imports)]
            use super::*;
            use ::std::ffi::{CString, c_char};
            use ::std::sync::OnceLock;
            use $crate::ffi as rt;

            pub type This = $engine;
            pub type Data = <This as $crate::Engine>::Data;
            pub type Session = rt::SessionHandle<This>;
            pub type Engine = rt::EngineHandle<This>;
            pub use rt::{P_BORROWED, PBuf};
            /// The symbol prefix.
            pub const PREFIX: &str = $p;

            static DESCRIBE: OnceLock<CString> = OnceLock::new();

            /// Static JSON: what the engine is and has.
            #[unsafe(export_name = concat!($p, "_describe"))]
            pub extern "C" fn describe() -> *const c_char {
                rt::describe::<This>(&DESCRIBE)
            }

            /// The data, from named buffers. NULL on failure.
            #[unsafe(export_name = concat!($p, "_data_new"))]
            pub extern "C" fn data_new(bufs: *const PBuf, n: usize) -> *const Data {
                rt::data_new::<This>(bufs, n)
            }

            /// After every session on it is freed.
            #[unsafe(export_name = concat!($p, "_data_free"))]
            pub extern "C" fn data_free(data: *const Data) {
                rt::data_free::<This>(data)
            }

            /// A session on `data` (NULL for an engine that needs none). NULL if `init_json` has no harness.
            #[unsafe(export_name = concat!($p, "_session_new"))]
            pub extern "C" fn session_new(data: *const Data, init_json: *const c_char) -> *mut Session {
                rt::session_new::<This>(data, init_json)
            }

            /// After every engine of it is freed.
            #[unsafe(export_name = concat!($p, "_session_free"))]
            pub extern "C" fn session_free(session: *mut Session) {
                rt::session_free(session)
            }

            /// One text field. NULL on failure.
            #[unsafe(export_name = concat!($p, "_new"))]
            pub extern "C" fn new(session: *mut Session) -> *mut Engine {
                rt::new(session)
            }

            #[unsafe(export_name = concat!($p, "_free"))]
            pub extern "C" fn free(engine: *mut Engine) {
                rt::free(engine)
            }

            /// One event in, one reply out.
            #[unsafe(export_name = concat!($p, "_send"))]
            pub extern "C" fn send(
                session: *mut Session,
                engine: *mut Engine,
                event_json: *const c_char,
            ) -> *const c_char {
                rt::send(session, engine, event_json)
            }
        }
    };
}
