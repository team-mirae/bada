//! bada's engine as WebAssembly, for the web harness (`harness/web`).
//!
//! The eight functions of the standard (§3) come from the engine crate as they are: `bada_describe`,
//! `bada_data_new`, `bada_data_free`, `bada_session_new`, `bada_session_free`, `bada_new`,
//! `bada_free` and `bada_send`. This crate adds only what a page needs to call them: a way to put
//! bytes in the module's memory. Strings come back as pointers to NUL-terminated UTF-8, which the
//! page reads from the same memory (`src/engine.js`).

/// The engine's C ABI. Naming it here links the engine in, and its exports with it.
pub use bada::ffi;

/// `len` bytes for the page to write into: an event's JSON, a buffer's name, a data file.
#[unsafe(no_mangle)]
pub extern "C" fn bada_web_alloc(len: usize) -> *mut u8 {
    let mut bytes = Vec::<u8>::with_capacity(len.max(1));
    let ptr = bytes.as_mut_ptr();
    std::mem::forget(bytes);
    ptr
}

/// Frees what `bada_web_alloc(len)` returned.
///
/// # Safety
/// `ptr` is NULL, or came from `bada_web_alloc(len)` with the same `len` and wasn't freed yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bada_web_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() {
        // SAFETY: by the caller.
        drop(unsafe { Vec::from_raw_parts(ptr, 0, len.max(1)) });
    }
}
