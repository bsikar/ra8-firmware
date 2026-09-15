// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

use std::path::Path;
use std::process::Command;

#[test]
fn zig_consumer_runs_in_rust_gate() {
    let manifest_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
    let target_dir = std::env::var_os("CARGO_TARGET_DIR")
        .map_or_else(|| manifest_dir.join("target"), std::path::PathBuf::from);
    let provider_target = target_dir.join("zig-consumer-provider");
    let cargo_status = Command::new(std::env::var_os("CARGO").unwrap_or_else(|| "cargo".into()))
        .args(["build", "--locked", "--lib"])
        .env("CARGO_TARGET_DIR", &provider_target)
        .current_dir(manifest_dir)
        .status()
        .expect("the pinned Cargo executable must be available to its test gate");
    assert!(
        cargo_status.success(),
        "Cargo did not build the Rust provider"
    );
    let rust_lib_dir = provider_target.join("debug");
    let status = Command::new("zig")
        .args([
            "build",
            "test",
            "--summary",
            "all",
            &format!("-Drust-lib-dir={}", rust_lib_dir.display()),
        ])
        .current_dir(manifest_dir.join("zig"))
        .status()
        .expect("the pinned Zig executable must be available to the Rust test gate");
    assert!(status.success(), "Zig consumer rejected the Cargo artifact");
}
