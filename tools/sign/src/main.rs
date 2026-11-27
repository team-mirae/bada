//! `bada-sign`: the release side of `bada update`'s check (the standard, §10.2).
//!
//!   bada-sign keygen          prints a new key pair: the secret (for the CI secret
//!                             BADA_UPDATE_SIGNING_KEY) and the public key (for cli/src/update.rs)
//!   bada-sign sign <file>…    writes <file>.sig, the file's Ed25519 signature (64 bytes), with the
//!                             secret key from $BADA_UPDATE_SIGNING_KEY (64 hex digits)
//!   bada-sign verify <public-hex> <file>   checks <file>.sig

use std::process::ExitCode;

use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn unhex<const N: usize>(text: &str) -> Option<[u8; N]> {
    let text = text.trim();
    if text.len() != 2 * N {
        return None;
    }
    let mut out = [0u8; N];
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(&text[2 * i..2 * i + 2], 16).ok()?;
    }
    Some(out)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("keygen") => {
            let key = SigningKey::generate(&mut rand_core::OsRng);
            println!("secret {}", hex(&key.to_bytes()));
            println!("public {}", hex(key.verifying_key().as_bytes()));
            ExitCode::SUCCESS
        }
        Some("sign") if args.len() > 1 => {
            let Some(secret) = std::env::var("BADA_UPDATE_SIGNING_KEY").ok().and_then(|s| unhex::<32>(&s)) else {
                eprintln!("BADA_UPDATE_SIGNING_KEY isn't 64 hex digits");
                return ExitCode::from(1);
            };
            let key = SigningKey::from_bytes(&secret);
            for file in &args[1..] {
                let Ok(bytes) = std::fs::read(file) else {
                    eprintln!("{file} can't be read");
                    return ExitCode::from(1);
                };
                if std::fs::write(format!("{file}.sig"), key.sign(&bytes).to_bytes()).is_err() {
                    eprintln!("{file}.sig can't be written");
                    return ExitCode::from(1);
                }
                println!("signed {file}");
            }
            ExitCode::SUCCESS
        }
        Some("verify") if args.len() == 3 => {
            let ok = (|| {
                let key = VerifyingKey::from_bytes(&unhex::<32>(&args[1])?).ok()?;
                let sig: [u8; 64] = std::fs::read(format!("{}.sig", args[2])).ok()?.try_into().ok()?;
                key.verify(&std::fs::read(&args[2]).ok()?, &Signature::from_bytes(&sig)).ok()
            })();
            println!("{}", if ok.is_some() { "good signature" } else { "BAD signature" });
            if ok.is_some() { ExitCode::SUCCESS } else { ExitCode::from(1) }
        }
        _ => {
            eprintln!("bada-sign keygen | sign <file>… | verify <public-hex> <file>");
            ExitCode::from(2)
        }
    }
}
