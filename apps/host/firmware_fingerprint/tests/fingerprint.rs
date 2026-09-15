// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

use std::ffi::OsString;
use std::io;

use firmware_fingerprint::{Fingerprint, fingerprint, run};

struct FailingWriter;

impl io::Write for FailingWriter {
    fn write(&mut self, _buffer: &[u8]) -> io::Result<usize> {
        Err(io::Error::new(io::ErrorKind::BrokenPipe, "closed"))
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

#[test]
fn fingerprints_empty_image() {
    assert_eq!(
        fingerprint(&[]),
        Fingerprint {
            bytes: 0,
            fnv1a64: 0xcbf2_9ce4_8422_2325,
        }
    );
}

#[test]
fn fingerprints_known_payload() {
    assert_eq!(
        fingerprint(b"hello"),
        Fingerprint {
            bytes: 5,
            fnv1a64: 0xa430_d846_80aa_bd0b,
        }
    );
}

#[test]
fn fingerprints_binary_boundary_values() {
    assert_eq!(fingerprint(&[0x00, 0xff]).fnv1a64, 0x0831_c907_b4ea_2b60);
}

#[test]
fn run_reports_injected_read_failure() {
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    let status = run(
        [OsString::from("missing.bin")],
        |_| Err(io::Error::new(io::ErrorKind::PermissionDenied, "denied")),
        &mut stdout,
        &mut stderr,
    );
    assert_eq!(status, 2);
    assert!(stdout.is_empty());
    assert_eq!(
        String::from_utf8(stderr).unwrap(),
        "firmware_fingerprint: missing.bin: denied\n"
    );
}

#[test]
fn run_reports_injected_output_failure() {
    let mut stderr = Vec::new();
    let mut stdout = FailingWriter;
    let status = run(
        [OsString::from("image.bin")],
        |_| Ok(vec![1]),
        &mut stdout,
        &mut stderr,
    );
    assert_eq!(status, 2);
    assert_eq!(stderr, b"firmware_fingerprint: failed to write output\n");
}

#[test]
fn run_rejects_missing_path() {
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    let status = run([], |_| Ok(Vec::new()), &mut stdout, &mut stderr);
    assert_eq!(status, 2);
    assert!(stdout.is_empty());
    assert_eq!(stderr, b"usage: firmware_fingerprint <firmware-image>\n");
}

#[test]
fn run_rejects_extra_argument_without_reading() {
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    let status = run(
        [OsString::from("one"), OsString::from("two")],
        |_| panic!("malformed arguments must not read a file"),
        &mut stdout,
        &mut stderr,
    );
    assert_eq!(status, 2);
    assert!(stdout.is_empty());
    assert_eq!(stderr, b"usage: firmware_fingerprint <firmware-image>\n");
}
