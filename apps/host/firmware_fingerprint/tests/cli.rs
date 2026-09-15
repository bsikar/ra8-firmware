// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

use std::fs;
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

fn unique_fixture(name: &str) -> std::path::PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system clock must follow the Unix epoch")
        .as_nanos();
    std::env::temp_dir().join(format!(
        "firmware-fingerprint-{}-{nonce}-{name}",
        std::process::id()
    ))
}

#[test]
fn cli_fingerprints_a_real_file() {
    let fixture = unique_fixture("success.bin");
    fs::write(&fixture, b"hello").expect("create CLI fixture");
    let output = Command::new(env!("CARGO_BIN_EXE_firmware_fingerprint"))
        .arg(&fixture)
        .output()
        .expect("run firmware_fingerprint");
    fs::remove_file(&fixture).expect("remove CLI fixture");

    assert!(output.status.success());
    assert_eq!(output.stdout, b"bytes=5\nfnv1a64=a430d84680aabd0b\n");
    assert!(output.stderr.is_empty());
}

#[test]
fn cli_rejects_missing_argument() {
    let output = Command::new(env!("CARGO_BIN_EXE_firmware_fingerprint"))
        .output()
        .expect("run firmware_fingerprint");
    assert_eq!(output.status.code(), Some(2));
    assert!(output.stdout.is_empty());
    assert_eq!(
        output.stderr,
        b"usage: firmware_fingerprint <firmware-image>\n"
    );
}

#[test]
fn cli_reports_missing_file() {
    let fixture = unique_fixture("absent.bin");
    let output = Command::new(env!("CARGO_BIN_EXE_firmware_fingerprint"))
        .arg(&fixture)
        .output()
        .expect("run firmware_fingerprint");
    assert_eq!(output.status.code(), Some(2));
    assert!(output.stdout.is_empty());
    assert!(
        String::from_utf8(output.stderr)
            .expect("diagnostic must be UTF-8")
            .contains("No such file or directory")
    );
}
