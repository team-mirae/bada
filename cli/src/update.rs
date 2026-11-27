//! `bada update` (the standard, §10.2): installs the newest release from `team-mirae/bada` on GitHub;
//! `--check` only looks. **Only when asked**: the input method never contacts GitHub by itself, and
//! this is the one command that does.
//!
//! A download is used only if its signature (`<file>.sig`, Ed25519, made in CI by `tools/sign`) checks
//! against the public key in this build. A build with no key refuses to install. Downloads go
//! through `curl`, which every supported system has.

use std::path::{Path, PathBuf};
use std::process::Command;

use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use serde_json::{Value, json};

use crate::edit::Answer;

pub const REPO: &str = "team-mirae/bada";

/// The release signing key's public half (`bada-sign keygen`), 64 hex digits. Empty: this build
/// can look for releases but not install one.
pub const PUBLIC_KEY: &str = "51509bf7c4ccc3db428707a6ca492a858f4eadf853c8b23f936d6c83cd7c27e9";

/// This platform's release file for `version`.
pub fn asset_name(version: &str) -> String {
    match crate::platform() {
        "mac" => format!("Bada-{version}.zip"),
        "windows" => format!("bada-{version}-setup.exe"),
        _ => format!("bada-{version}-linux-x86_64.tar.gz"),
    }
}

/// `a.b.c` as numbers, for comparing.
pub fn version_key(v: &str) -> Vec<u64> {
    v.trim_start_matches('v').split('.').map(|p| p.parse().unwrap_or(0)).collect()
}

fn curl(args: &[&str]) -> Result<Vec<u8>, String> {
    let out = Command::new("curl")
        .args(["-fsSL", "--proto", "=https", "-H", "User-Agent: bada-cli"])
        .args(args)
        .output()
        .map_err(|e| format!("curl can't be run: {e}"))?;
    if out.status.success() { Ok(out.stdout) } else { Err(String::from_utf8_lossy(&out.stderr).trim().to_string()) }
}

struct Release {
    version: String,
    asset: Option<(String, String)>,
    signature: Option<String>,
}

fn latest() -> Result<Release, String> {
    let body = curl(&["-H", "Accept: application/vnd.github+json", &format!("https://api.github.com/repos/{REPO}/releases/latest")])?;
    let v: Value = serde_json::from_slice(&body).map_err(|e| format!("GitHub's answer isn't JSON: {e}"))?;
    let tag = v["tag_name"].as_str().ok_or("the latest release has no tag")?;
    let version = tag.trim_start_matches('v').to_string();
    let name = asset_name(&version);
    let find = |n: &str| {
        v["assets"].as_array()?.iter().find(|a| a["name"] == n).and_then(|a| a["browser_download_url"].as_str()).map(String::from)
    };
    Ok(Release {
        asset: find(&name).map(|url| (name.clone(), url)),
        signature: find(&format!("{name}.sig")),
        version,
    })
}

/// Checks `file` against `sig` with the key in this build.
pub fn verify(key_hex: &str, file: &[u8], sig: &[u8]) -> Result<(), String> {
    let bytes: Vec<u8> = (0..key_hex.len() / 2).filter_map(|i| u8::from_str_radix(key_hex.get(2 * i..2 * i + 2)?, 16).ok()).collect();
    let key: [u8; 32] = bytes.try_into().map_err(|_| "this build's update key isn't 32 bytes".to_string())?;
    let key = VerifyingKey::from_bytes(&key).map_err(|_| "this build's update key isn't an Ed25519 key".to_string())?;
    let sig: [u8; 64] = sig.try_into().map_err(|_| "the signature isn't 64 bytes".to_string())?;
    key.verify(file, &Signature::from_bytes(&sig)).map_err(|_| "the signature doesn't match: the download is not used".to_string())
}

pub fn run(check_only: bool) -> Answer {
    let release = match latest() {
        Ok(r) => r,
        Err(e) => return Answer::failed("no-release", format!("the latest release can't be read from GitHub: {e}")),
    };
    let current = crate::VERSION;
    let newer = version_key(&release.version) > version_key(current);
    let info = json!({"installed": current, "latest": release.version, "newer": newer,
                      "file": release.asset.as_ref().map(|a| &a.0), "signed": release.signature.is_some()});
    if check_only || !newer {
        let text = if newer {
            format!("bada {} is out (this is {current}): `bada update` installs it.", release.version)
        } else if version_key(current) > version_key(&release.version) {
            format!("This is bada {current}, newer than the latest release ({}).", release.version)
        } else {
            format!("bada {current} is the newest.")
        };
        return Answer::ok(info, text);
    }
    if PUBLIC_KEY.is_empty() {
        return Answer::failed("no-update-key", format!("bada {} is out, but this build has no update key, so it can't check a download: install it from https://github.com/{REPO}/releases", release.version));
    }
    let (Some((name, url)), Some(sig_url)) = (&release.asset, &release.signature) else {
        return Answer::failed("no-file", format!("release {} has no signed file for this platform", release.version));
    };
    let dir = std::env::temp_dir().join(format!("bada-update-{}", release.version));
    let _ = std::fs::create_dir_all(&dir);
    let file = dir.join(name);
    let downloaded = (|| -> Result<(Vec<u8>, Vec<u8>), String> {
        let bytes = curl(&[url])?;
        let sig = curl(&[sig_url])?;
        Ok((bytes, sig))
    })();
    let (bytes, sig) = match downloaded {
        Ok(d) => d,
        Err(e) => return Answer::failed("not-downloaded", e),
    };
    if let Err(e) = verify(PUBLIC_KEY, &bytes, &sig) {
        return Answer::failed("bad-signature", e);
    }
    if let Err(e) = std::fs::write(&file, &bytes) {
        return Answer::failed("not-written", e.to_string());
    }
    match install(&file) {
        Ok(text) => Answer::ok(info, text),
        Err(e) => Answer::failed("not-installed", e),
    }
}

#[cfg(target_os = "macos")]
fn install(zip: &Path) -> Result<String, String> {
    let home = std::env::var("HOME").map_err(|_| "HOME isn't set")?;
    let input_methods = PathBuf::from(home).join("Library/Input Methods");
    let unpacked = zip.with_extension("d");
    let _ = std::fs::remove_dir_all(&unpacked);
    run_ok("ditto", &["-x", "-k", &zip.to_string_lossy(), &unpacked.to_string_lossy()])?;
    let app = unpacked.join("Bada.app");
    if !app.is_dir() {
        return Err("the download has no Bada.app".into());
    }
    let _ = Command::new("xattr").args(["-dr", "com.apple.quarantine"]).arg(&app).status();
    let target = input_methods.join("Bada.app");
    let old = input_methods.join(".Bada.app.old");
    let _ = std::fs::remove_dir_all(&old);
    let _ = Command::new("killall").arg("Bada").status();
    if target.exists() {
        std::fs::rename(&target, &old).map_err(|e| format!("the installed app can't be moved aside: {e}"))?;
    }
    if let Err(e) = std::fs::rename(&app, &target) {
        let _ = std::fs::rename(&old, &target);
        return Err(format!("the new app can't be put in place: {e}"));
    }
    let _ = std::fs::remove_dir_all(&old);
    let _ = Command::new("open").args(["-g", "-b", "com.teammirae.inputmethod.Bada"]).status();
    Ok(format!("Installed into {}; the input method was restarted.", target.display()))
}

#[cfg(windows)]
fn install(setup: &Path) -> Result<String, String> {
    run_ok(&setup.to_string_lossy(), &["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"])?;
    Ok("Installed.".into())
}

#[cfg(all(not(target_os = "macos"), not(windows)))]
fn install(tarball: &Path) -> Result<String, String> {
    // The package lays bada out under /usr: that needs root, which this command doesn't take.
    Ok(format!("Downloaded and checked: {}\nTo install it: sudo tar -C / -xzf {}", tarball.display(), tarball.display()))
}

fn run_ok(program: &str, args: &[&str]) -> Result<(), String> {
    let status = Command::new(program).args(args).status().map_err(|e| format!("{program} can't be run: {e}"))?;
    if status.success() { Ok(()) } else { Err(format!("{program} failed ({status})")) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions_compare_as_numbers() {
        assert!(version_key("0.10.0") > version_key("0.9.9"));
        assert!(version_key("v0.3.0") > version_key("0.2.1"));
        assert_eq!(version_key("0.3.0"), version_key("v0.3.0"));
    }

    #[test]
    fn a_signature_must_match() {
        // RFC 8032's first test vector: the empty message.
        let public = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
        let sig: Vec<u8> = (0..64).map(|i| u8::from_str_radix(&"e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"[2 * i..2 * i + 2], 16).unwrap()).collect();
        assert!(verify(public, b"", &sig).is_ok());
        assert!(verify(public, b"x", &sig).is_err());
        assert!(verify("", b"", &sig).is_err());
    }
}
