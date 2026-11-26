//! The published state (the standard, §6.7), read as any other program reads it: a file on macOS
//! and Linux, a block of shared memory on Windows. Read-only: the command line can't change it.

use serde_json::Value;

use crate::folder::Folder;

/// The state now, or null when nothing is published: the input method isn't running, or has
/// published nothing yet.
pub fn read(folder: &Folder) -> Value {
    text(folder).and_then(|text| serde_json::from_str(&text).ok()).unwrap_or(Value::Null)
}

#[cfg(target_os = "macos")]
fn text(folder: &Folder) -> Option<String> {
    // The Mac's input method writes it into the settings folder.
    std::fs::read_to_string(folder.dir.join("state.json")).ok()
}

#[cfg(all(unix, not(target_os = "macos")))]
fn text(_folder: &Folder) -> Option<String> {
    // $XDG_RUNTIME_DIR/bada/state.json, or /tmp/bada-<uid>/state.json without one
    // (harness/linux/src/common/publish.cpp).
    unsafe extern "C" {
        fn getuid() -> u32;
    }
    let dir = match std::env::var("XDG_RUNTIME_DIR").ok().filter(|dir| dir.starts_with('/')) {
        Some(run) => std::path::PathBuf::from(run).join("bada"),
        None => std::path::PathBuf::from("/tmp").join(format!("bada-{}", unsafe { getuid() })),
    };
    std::fs::read_to_string(dir.join("state.json")).ok()
}

#[cfg(windows)]
fn text(_folder: &Folder) -> Option<String> {
    shared::read()
}

/// The size of the Windows block (harness/windows/src/core/state_block.h).
pub const BLOCK_SIZE: usize = 65_536;
const HEADER: usize = 32;
const MAGIC: u32 = 0x3253_4442; // "BDS2"

/// The text of a copy of the Windows state block, whose layout is:
/// magic, version, capacity, sequence, length, process id, thread id, flags (u32 each, little
/// endian), then the text. None: not a block, nothing published, or caught while being written
/// (an odd sequence).
pub fn block(bytes: &[u8]) -> Option<String> {
    let field = |at: usize| bytes.get(at..at + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]));
    if bytes.len() <= HEADER || field(0)? != MAGIC || field(12)? & 1 == 1 {
        return None;
    }
    let length = field(16)? as usize;
    if length == 0 || length > bytes.len() - HEADER {
        return None;
    }
    String::from_utf8(bytes[HEADER..HEADER + length].to_vec()).ok()
}

#[cfg(windows)]
mod shared {
    use std::ffi::c_void;

    use super::{BLOCK_SIZE, block};

    // Local\com.teammirae.bada.state.v2.{CLSID} (harness/windows/src/tip/guids.h).
    const NAME: &str = "Local\\com.teammirae.bada.state.v2.{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}";
    const FILE_MAP_READ: u32 = 4;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn OpenFileMappingW(access: u32, inherit: i32, name: *const u16) -> *mut c_void;
        fn MapViewOfFile(mapping: *mut c_void, access: u32, high: u32, low: u32, bytes: usize) -> *mut c_void;
        fn UnmapViewOfFile(view: *const c_void) -> i32;
        fn CloseHandle(handle: *mut c_void) -> i32;
    }

    /// Reads the block as the standard says a reader does: the sequence, a copy of the rest, the
    /// sequence again, and once more from the start if it was odd or changed.
    pub fn read() -> Option<String> {
        let name: Vec<u16> = NAME.encode_utf16().chain([0]).collect();
        // SAFETY: the name is NUL-terminated; a null handle or view is checked before use; the view
        // is BLOCK_SIZE bytes, as the writer made the mapping, and is only read.
        unsafe {
            let mapping = OpenFileMappingW(FILE_MAP_READ, 0, name.as_ptr());
            if mapping.is_null() {
                return None;
            }
            let view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, BLOCK_SIZE);
            let mut text = None;
            if !view.is_null() {
                let sequence = view.cast::<u8>().add(12).cast::<u32>();
                for _ in 0..64 {
                    let before = sequence.read_volatile();
                    if before & 1 == 1 {
                        std::thread::yield_now();
                        continue;
                    }
                    let mut copy = vec![0u8; BLOCK_SIZE];
                    std::ptr::copy_nonoverlapping(view.cast::<u8>(), copy.as_mut_ptr(), BLOCK_SIZE);
                    std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);
                    if sequence.read_volatile() == before {
                        text = block(&copy);
                        break;
                    }
                }
                UnmapViewOfFile(view);
            }
            CloseHandle(mapping);
            text
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn made(sequence: u32, text: &str) -> Vec<u8> {
        let mut bytes = vec![0u8; 256];
        for (at, value) in [(0, MAGIC), (4, 2), (8, 224), (12, sequence), (16, text.len() as u32)] {
            bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
        }
        bytes[HEADER..HEADER + text.len()].copy_from_slice(text.as_bytes());
        bytes
    }

    #[test]
    fn the_windows_block_is_read_as_its_writer_lays_it_out() {
        let state = r#"{"format":"bada-compatible-state/2","mode":"henkan","staged":"한"}"#;
        assert_eq!(block(&made(4, state)).as_deref(), Some(state));
        assert_eq!(block(&made(5, state)), None, "an odd sequence: it is being written");
        assert_eq!(block(&made(4, "")), None, "nothing is published");
        assert_eq!(block(&[0u8; 256]), None, "not a block");
        assert_eq!(block(&[]), None);
        let mut long = made(4, state);
        long[16..20].copy_from_slice(&9_999u32.to_le_bytes());
        assert_eq!(block(&long), None, "a length past the block");
    }
}
