//! Where the settings page's files are (`settings/README.md`): a folder beside the binary, as each
//! platform's build installs it, or the repository's own when the command line is run from it.

use std::path::{Path, PathBuf};

/// The folder of the page's files: `--page`, `$BADA_SETTINGS_PAGE`, then beside the binary
/// (`settings`, the Mac bundle's `../Resources/settings`, a prefix's `../share/bada/settings`),
/// and last the repository's `settings/src`. None: no folder has an `index.html`.
pub fn find(named: Option<&Path>) -> Option<PathBuf> {
    let has_page = |dir: &Path| dir.join("index.html").is_file();
    if let Some(dir) = named {
        return has_page(dir).then(|| dir.to_path_buf());
    }
    if let Some(dir) = std::env::var_os("BADA_SETTINGS_PAGE").filter(|dir| !dir.is_empty()) {
        let dir = PathBuf::from(dir);
        return has_page(&dir).then_some(dir);
    }
    let exe = std::env::current_exe().ok().and_then(|exe| exe.canonicalize().ok());
    let beside =
        exe.as_deref().and_then(Path::parent).into_iter().flat_map(|dir| {
            [dir.join("settings"), dir.join("../Resources/settings"), dir.join("../share/bada/settings")]
        });
    let repository = Path::new(env!("CARGO_MANIFEST_DIR")).join("../settings/src");
    beside.chain([repository]).find(|dir| has_page(dir))
}

/// The `Content-Type` of a file of the page, by its name.
pub fn content_type(path: &Path) -> &'static str {
    match path.extension().and_then(|e| e.to_str()).unwrap_or("") {
        "html" => "text/html; charset=utf-8",
        "css" => "text/css; charset=utf-8",
        "js" | "mjs" => "text/javascript; charset=utf-8",
        "json" => "application/json; charset=utf-8",
        "svg" => "image/svg+xml",
        "wasm" => "application/wasm",
        "png" => "image/png",
        "txt" => "text/plain; charset=utf-8",
        _ => "application/octet-stream",
    }
}

/// The file a request's path names, inside the page's folder. `/` is `index.html`. A path that
/// could leave the folder (`..`, a hidden name, anything but letters, digits, `.`, `-`, `_`) names
/// nothing.
pub fn file(page: &Path, path: &str) -> Option<PathBuf> {
    let path = path.split(['?', '#']).next().unwrap_or("").trim_start_matches('/');
    let path = if path.is_empty() { "index.html" } else { path };
    let plain = |part: &str| {
        !part.is_empty()
            && !part.starts_with('.')
            && part.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b'_'))
    };
    if !path.split('/').all(plain) {
        return None;
    }
    let file = page.join(path);
    file.is_file().then_some(file)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_files_inside_the_folder() {
        let page = Path::new(env!("CARGO_MANIFEST_DIR"));
        assert_eq!(file(page, "/Cargo.toml"), Some(page.join("Cargo.toml")));
        assert_eq!(file(page, "/src/main.rs?v=1#top"), Some(page.join("src/main.rs")));
        for bad in [
            "/../Cargo.toml",
            "/src/../../Cargo.toml",
            "/.git/config",
            "/src//main.rs",
            "/src/%2e%2e/x",
            "/nothing.js",
            "/src",
        ] {
            assert_eq!(file(page, bad), None, "{bad}");
        }
        assert_eq!(content_type(Path::new("a/page.js")), "text/javascript; charset=utf-8");
        assert_eq!(content_type(Path::new("bada.wasm")), "application/wasm");
        assert_eq!(content_type(Path::new("index.html")), "text/html; charset=utf-8");
    }
}
