//! bada, the command line (the standard, §10.2): a harness with no text field. It links the
//! engine to check what it writes, reads and writes the settings folder, and serves the settings
//! page (`settings/README.md`) on this machine.
//!
//! What is here now: `version`, `settings` (the page's host), `settings validate` and `state`.

pub mod check;
pub mod folder;
pub mod keys;
pub mod page;
pub mod server;
pub mod state;

/// bada's one version (the repository's `VERSION`).
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// The platform, as the page names it.
pub fn platform() -> &'static str {
    if cfg!(target_os = "macos") {
        "mac"
    } else if cfg!(windows) {
        "windows"
    } else {
        "linux"
    }
}

/// The id of the input method's harness on this platform (§12): its part of `harness` in the
/// settings file is the one the page shows.
pub fn harness_id() -> String {
    format!("com.teammirae.bada.{}", platform())
}

/// The command line's own id, for the engine's session.
pub const CLI_ID: &str = "com.teammirae.bada.cli";
