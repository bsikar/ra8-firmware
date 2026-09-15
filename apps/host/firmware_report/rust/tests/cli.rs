// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

use std::fs;

#[test]
fn rejects_missing_path() {
    assert_eq!(
        firmware_report_provider::run(&["firmware_report".into()]),
        Err("usage")
    );
}

#[test]
fn rejects_extra_path() {
    assert_eq!(
        firmware_report_provider::run(&["firmware_report".into(), "a".into(), "b".into()]),
        Err("usage")
    );
}

#[test]
fn rejects_missing_file() {
    assert_eq!(
        firmware_report_provider::run(&["firmware_report".into(), "/definitely/missing".into()]),
        Err("cannot open input")
    );
}

#[test]
fn renders_report() {
    let path = std::env::temp_dir().join(format!("firmware-report-{}.bin", std::process::id()));
    fs::write(&path, b"hello").expect("fixture write");
    let output =
        firmware_report_provider::run(&["firmware_report".into(), path.clone().into_os_string()]);
    fs::remove_file(path).expect("fixture cleanup");
    assert_eq!(
        output,
        Ok("bytes=5\nzero=0\nerased=0\nfnv1a64=a430d84680aabd0b\n".into())
    );
}
