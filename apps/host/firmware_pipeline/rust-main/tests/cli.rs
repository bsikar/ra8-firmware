// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

use std::fs;

#[test]
fn rejects_missing_path() {
    assert_eq!(
        firmware_pipeline_rust_main::run(&["pipeline".into()]),
        Err("usage")
    );
}

#[test]
fn rejects_extra_path() {
    assert_eq!(
        firmware_pipeline_rust_main::run(&["pipeline".into(), "a".into(), "b".into()]),
        Err("usage")
    );
}

#[test]
fn rejects_missing_file() {
    assert_eq!(
        firmware_pipeline_rust_main::run(&["pipeline".into(), "/definitely/missing".into()]),
        Err("cannot read bounded input")
    );
}

#[test]
fn runs_all_language_stages() {
    let path = std::env::temp_dir().join(format!("firmware-pipeline-{}.bin", std::process::id()));
    fs::write(&path, b"hello").expect("fixture write");
    let output =
        firmware_pipeline_rust_main::run(&["pipeline".into(), path.clone().into_os_string()]);
    fs::remove_file(path).expect("fixture cleanup");
    assert_eq!(
        output,
        Ok(
            "bytes=5\nzero=0\nerased=0\nfnv1a64=a430d84680aabd0b\nzig_xor8=62\nzig_stage=5a\n"
                .into()
        )
    );
}
