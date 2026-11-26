//! The settings folder (the standard, §10.1): `settings.json`, `layouts/*.json`, `dictionary.json`
//! and `history/`. Every write is checked first, goes to a temporary file and is renamed, so the
//! input method that watches the folder never reads half a file. Names are checked, not trusted:
//! nothing outside the folder is read or written.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use bada_compat::Problem;
use serde_json::{Value, json};

use crate::check::{self, Checked};
use crate::keys::is_layout_id;

/// How many replaced `settings.json` files `history/` keeps.
pub const HISTORY: usize = 20;

/// A file of the folder that can be written or removed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Name {
    Settings,
    Dictionary,
    /// `layouts/<id>.json`.
    Layout(String),
}

impl Name {
    /// `settings.json`, `dictionary.json` or `layouts/<id>.json` with a slug for an id (§7.1).
    pub fn parse(name: &str) -> Option<Name> {
        match name {
            "settings.json" => Some(Name::Settings),
            "dictionary.json" => Some(Name::Dictionary),
            _ => {
                let id = name.strip_prefix("layouts/")?.strip_suffix(".json")?;
                is_layout_id(id).then(|| Name::Layout(id.to_string()))
            }
        }
    }

    fn path(&self, dir: &Path) -> PathBuf {
        match self {
            Name::Settings => dir.join("settings.json"),
            Name::Dictionary => dir.join("dictionary.json"),
            Name::Layout(id) => dir.join("layouts").join(format!("{id}.json")),
        }
    }
}

/// `settings-<digits>-<digits>.json`: a file of `history/`.
pub fn is_history_name(name: &str) -> bool {
    let digits = |part: &str| !part.is_empty() && part.len() <= 16 && part.bytes().all(|b| b.is_ascii_digit());
    name.strip_prefix("settings-")
        .and_then(|rest| rest.strip_suffix(".json"))
        .and_then(|stamp| stamp.split_once('-'))
        .is_some_and(|(date, time)| digits(date) && digits(time))
}

/// A history file's name for a moment: `settings-<yyyymmdd>-<hhmmss>.json`, in UTC.
pub fn history_name(at: SystemTime) -> String {
    let secs = at.duration_since(UNIX_EPOCH).unwrap_or_default().as_secs() as i64;
    let (days, of_day) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    // Days to a date in the Gregorian calendar (Howard Hinnant's civil_from_days).
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "settings-{year:04}{month:02}{day:02}-{:02}{:02}{:02}.json",
        of_day / 3_600,
        of_day % 3_600 / 60,
        of_day % 60
    )
}

/// The answer to a write, a removal or an undo.
#[derive(Debug)]
pub struct Outcome {
    pub ok: bool,
    pub problems: Vec<Problem>,
}

impl Outcome {
    fn refused(path: &str, code: &str, message: impl Into<String>) -> Outcome {
        Outcome { ok: false, problems: vec![Problem::error(path, code, message)] }
    }

    fn failed(what: &str, error: &io::Error) -> Outcome {
        Outcome::refused("", "not-written", format!("{what}: {error}"))
    }
}

pub struct Folder {
    pub dir: PathBuf,
    /// The folder as the user would write it (`~/.config/bada`).
    pub shown: String,
}

fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
}

impl Folder {
    /// A folder named outright: a test's, or one an environment variable names.
    pub fn at(dir: impl Into<PathBuf>) -> Folder {
        let dir = dir.into();
        let shown = dir.display().to_string();
        Folder { dir, shown }
    }

    /// The platform's folder. `BADA_CONFIG_DIR` names another, as it does for the Windows and
    /// Linux input methods, and `BADA_HOME` as it does for the Mac's.
    pub fn from_env() -> Folder {
        if let Some(dir) = env("BADA_CONFIG_DIR").or_else(|| env("BADA_HOME")) {
            return Folder::at(dir);
        }
        let home = env("HOME").map(PathBuf::from).unwrap_or_default();
        if cfg!(target_os = "macos") {
            let dir = home.join("Library").join("Application Support").join("bada");
            Folder { dir, shown: "~/Library/Application Support/bada".into() }
        } else if cfg!(windows) {
            match env("APPDATA") {
                Some(roaming) => Folder { dir: PathBuf::from(roaming).join("bada"), shown: "%APPDATA%\\bada".into() },
                None => Folder::at(PathBuf::from("bada")),
            }
        } else {
            match env("XDG_CONFIG_HOME").filter(|dir| dir.starts_with('/')) {
                Some(config) => Folder::at(PathBuf::from(config).join("bada")),
                None => Folder { dir: home.join(".config").join("bada"), shown: "~/.config/bada".into() },
            }
        }
    }

    fn modified(path: &Path) -> u64 {
        let at = fs::metadata(path).and_then(|m| m.modified()).unwrap_or(UNIX_EPOCH);
        at.duration_since(UNIX_EPOCH).unwrap_or_default().as_secs()
    }

    fn file(path: &Path) -> Value {
        match fs::read_to_string(path) {
            Ok(text) => json!({"text": text, "modified": Folder::modified(path)}),
            Err(_) => Value::Null,
        }
    }

    /// The names of a subfolder's `.json` files, sorted.
    fn names(&self, sub: &str) -> Vec<String> {
        let mut names: Vec<String> = fs::read_dir(self.dir.join(sub))
            .into_iter()
            .flatten()
            .flatten()
            .filter(|entry| entry.file_type().is_ok_and(|t| t.is_file()))
            .filter_map(|entry| entry.file_name().into_string().ok())
            .filter(|name| name.ends_with(".json"))
            .collect();
        names.sort();
        names
    }

    /// The user's own layouts: `(id, text)`, the id being the file's name.
    pub fn layouts(&self) -> Vec<(String, String)> {
        let ids = self.names("layouts").into_iter().filter_map(|name| name.strip_suffix(".json").map(String::from));
        ids.filter(|id| is_layout_id(id))
            .filter_map(|id| {
                let text = fs::read_to_string(Name::Layout(id.clone()).path(&self.dir)).ok()?;
                Some((id, text))
            })
            .collect()
    }

    /// The files of `history/`, newest first.
    pub fn history(&self) -> Vec<String> {
        let mut names: Vec<String> = self.names("history").into_iter().filter(|name| is_history_name(name)).collect();
        names.reverse();
        names
    }

    /// Everything the page shows of the folder (`settings/README.md`, `files()`).
    pub fn files(&self) -> Value {
        let layouts: Vec<Value> = self
            .layouts()
            .into_iter()
            .map(|(id, text)| {
                let modified = Folder::modified(&Name::Layout(id.clone()).path(&self.dir));
                json!({"id": id, "text": text, "modified": modified})
            })
            .collect();
        let history: Vec<Value> = self
            .history()
            .into_iter()
            .map(|name| {
                let modified = Folder::modified(&self.dir.join("history").join(&name));
                json!({"name": name, "modified": modified})
            })
            .collect();
        json!({
            "settings": Folder::file(&Name::Settings.path(&self.dir)),
            "dictionary": Folder::file(&Name::Dictionary.path(&self.dir)),
            "layouts": layouts,
            "history": history,
        })
    }

    /// Checks a file as `write` would, by what it is.
    pub fn check(&self, name: &Name, text: &str) -> Checked {
        match name {
            Name::Settings => check::settings(self, text),
            Name::Dictionary => check::dictionary(self, text),
            Name::Layout(id) => {
                let (mut checked, own) = check::layout(self, text);
                if own.as_deref().is_some_and(|own| own != id) {
                    let message = format!("the file is layouts/{id}.json, so its id is {id}");
                    checked.problems.push(Problem::error("id", "bad-value", message));
                }
                checked
            }
        }
    }

    /// Keeps the `settings.json` that is about to be replaced, and only the newest [`HISTORY`].
    fn keep(&self, now: SystemTime) -> io::Result<()> {
        let current = Name::Settings.path(&self.dir);
        if !current.exists() {
            return Ok(());
        }
        let history = self.dir.join("history");
        fs::create_dir_all(&history)?;
        // Two saves in one second: the later one takes the next second's name.
        let mut at = now;
        while history.join(history_name(at)).exists() {
            at += Duration::from_secs(1);
        }
        fs::copy(&current, history.join(history_name(at)))?;
        for old in self.history().into_iter().skip(HISTORY) {
            fs::remove_file(history.join(old))?;
        }
        Ok(())
    }

    /// Writes a file: checked, refused with an error-level problem, then a temporary file and a
    /// rename. A `settings.json` that is replaced goes into `history/` first.
    pub fn write(&self, name: &str, text: &str) -> Outcome {
        self.write_at(name, text, SystemTime::now())
    }

    /// [`Folder::write`], at a given moment: the history file's name.
    pub fn write_at(&self, name: &str, text: &str, now: SystemTime) -> Outcome {
        let Some(name) = Name::parse(name) else {
            return Outcome::refused(
                "name",
                "bad-value",
                "a file is settings.json, dictionary.json or layouts/<id>.json",
            );
        };
        let checked = self.check(&name, text);
        if !checked.ok() {
            return Outcome { ok: false, problems: checked.problems };
        }
        let path = name.path(&self.dir);
        // The same text again changes nothing: no write, and nothing new in the history.
        if fs::read_to_string(&path).is_ok_and(|current| current == text) {
            return Outcome { ok: true, problems: checked.problems };
        }
        let written = (|| {
            fs::create_dir_all(path.parent().unwrap_or(&self.dir))?;
            if name == Name::Settings {
                self.keep(now)?;
            }
            let mut tmp = path.clone().into_os_string();
            tmp.push(".tmp");
            fs::write(&tmp, text)?;
            fs::rename(&tmp, &path)
        })();
        match written {
            Ok(()) => Outcome { ok: true, problems: checked.problems },
            Err(e) => Outcome::failed("the file couldn't be written", &e),
        }
    }

    /// Removes `dictionary.json` or a layout of the user's. A file that isn't there is removed already.
    pub fn remove(&self, name: &str) -> Outcome {
        let path = match Name::parse(name) {
            Some(name @ (Name::Dictionary | Name::Layout(_))) => name.path(&self.dir),
            _ => return Outcome::refused("name", "bad-value", "only dictionary.json or layouts/<id>.json is removed"),
        };
        match fs::remove_file(&path) {
            Ok(()) => Outcome { ok: true, problems: vec![] },
            Err(e) if e.kind() == io::ErrorKind::NotFound => Outcome { ok: true, problems: vec![] },
            Err(e) => Outcome::failed("the file couldn't be removed", &e),
        }
    }

    /// Puts a file of `history/` back as `settings.json`, through [`Folder::write`]: the file it
    /// replaces goes into the history in its turn.
    pub fn undo(&self, name: &str) -> Outcome {
        if !is_history_name(name) {
            return Outcome::refused("name", "bad-value", "a history file is settings-<date>-<time>.json");
        }
        match fs::read_to_string(self.dir.join("history").join(name)) {
            Ok(text) => self.write("settings.json", &text),
            Err(e) => Outcome::failed("the history file couldn't be read", &e),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_are_checked() {
        assert_eq!(Name::parse("settings.json"), Some(Name::Settings));
        assert_eq!(Name::parse("dictionary.json"), Some(Name::Dictionary));
        assert_eq!(Name::parse("layouts/ko-my-2-set.json"), Some(Name::Layout("ko-my-2-set".into())));
        for bad in [
            "",
            "../x",
            "state.json",
            "history/settings-1-1.json",
            "layouts/../../x.json",
            "layouts/x.json",
            "layouts/ko-2-set",
            "layouts/Ko-2-set.json",
            "layouts/ko-2-set.json/..",
            "/etc/passwd",
            "layouts\\ko-2-set.json",
        ] {
            assert_eq!(Name::parse(bad), None, "{bad}");
        }
        assert!(is_history_name("settings-20261124-085514.json"));
        for bad in
            ["settings.json", "settings-1.json", "settings-a-1.json", "settings-1-1.json/..", "../settings-1-1.json"]
        {
            assert!(!is_history_name(bad), "{bad}");
        }
    }

    #[test]
    fn history_names_are_the_date_and_the_time() {
        assert_eq!(history_name(UNIX_EPOCH), "settings-19700101-000000.json");
        assert_eq!(history_name(UNIX_EPOCH + Duration::from_secs(1_795_650_776)), "settings-20261125-235256.json");
        assert_eq!(history_name(UNIX_EPOCH + Duration::from_secs(951_782_399)), "settings-20000228-235959.json");
        assert_eq!(history_name(UNIX_EPOCH + Duration::from_secs(951_782_400)), "settings-20000229-000000.json");
        assert!(is_history_name(&history_name(SystemTime::now())));
    }
}
