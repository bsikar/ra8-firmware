// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Link the Rust consumer to the Zig library built from the shared fixture graph.

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

const LIB_DIR_ENV: &str = "RA8_ABI_FIXTURE_LIB_DIR";

fn build_zig_library(manifest_dir: &Path, output_dir: &Path) -> PathBuf {
    let fixture_root = manifest_dir
        .parent()
        .expect("Rust crate must remain below the ABI fixture root");
    let prefix = output_dir.join("zig-prefix");
    let cache = output_dir.join("zig-cache");
    let global_cache = output_dir.join("zig-global-cache");
    let status = Command::new(env::var_os("ZIG").unwrap_or_else(|| "zig".into()))
        .args(["build", "-Doptimize=Debug", "--prefix"])
        .arg(&prefix)
        .arg("--cache-dir")
        .arg(cache)
        .arg("--global-cache-dir")
        .arg(global_cache)
        .current_dir(fixture_root)
        .status()
        .expect("failed to execute Zig for the Rust ABI consumer");
    assert!(status.success(), "Zig ABI fixture build failed");
    prefix.join("lib")
}

fn main() {
    println!("cargo:rerun-if-env-changed={LIB_DIR_ENV}");
    println!("cargo:rerun-if-changed=../build.zig");
    // Directory watches cover future transitive Zig sources and public headers
    // without watching rust/target output and retriggering Cargo forever.
    println!("cargo:rerun-if-changed=../src");
    println!("cargo:rerun-if-changed=../inc");

    let manifest_dir =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("Cargo manifest dir"));
    let output_dir = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo output dir"));
    let library_dir = env::var_os(LIB_DIR_ENV).map_or_else(
        || build_zig_library(&manifest_dir, &output_dir),
        PathBuf::from,
    );
    println!("cargo:rustc-link-search=native={}", library_dir.display());
    println!("cargo:rustc-link-lib=static=ra8_abi_fixture");
}
