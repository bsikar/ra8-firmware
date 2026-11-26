// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Link `libra8_box.a` on host targets (`cfg!(not(target_os = "none"))`) when
//! running native Rust tests and Clippy checks. Bare-metal ARM firmware images
//! link `libra8_box_pair_rs` together with `libra8_box.a` through the root Zig
//! build graph instead.

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

const LIB_DIR_ENV: &str = "RA8_BOX_LIB_DIR";

fn build_zig_box_library(manifest_dir: &Path, output_dir: &Path) -> PathBuf {
    let repo_root = manifest_dir
        .ancestors()
        .nth(4)
        .expect("ra8_box_pair_rs must stay under examples/ek_ra8d2/<tier>/");
    let box_dir = repo_root.join("libs").join("ra8_box");
    let prefix = output_dir.join("zig-prefix");
    let cache = output_dir.join("zig-cache");
    let global_cache = output_dir.join("zig-global-cache");
    let zig = env::var_os("ZIG").unwrap_or_else(|| "zig".into());
    let status = Command::new(zig)
        .args(["build", "-Doptimize=Debug", "--prefix"])
        .arg(&prefix)
        .arg("--cache-dir")
        .arg(cache)
        .env("ZIG_GLOBAL_CACHE_DIR", global_cache)
        .current_dir(&box_dir)
        .status()
        .expect("failed to execute Zig to build libra8_box.a");
    assert!(status.success(), "Zig build of libs/ra8_box failed");
    prefix.join("lib")
}

fn main() {
    println!("cargo:rerun-if-env-changed={LIB_DIR_ENV}");
    println!("cargo:rerun-if-env-changed=ZIG");
    println!("cargo:rerun-if-changed=../../../../libs/ra8_box/build.zig");
    println!("cargo:rerun-if-changed=../../../../libs/ra8_box/src");
    println!("cargo:rerun-if-changed=../../../../libs/ra8_box/inc");

    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("none") {
        return;
    }

    let manifest_dir =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("Cargo manifest dir"));
    let output_dir = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo output dir"));
    let library_dir = env::var_os(LIB_DIR_ENV).map_or_else(
        || build_zig_box_library(&manifest_dir, &output_dir),
        PathBuf::from,
    );
    println!("cargo:rustc-link-search=native={}", library_dir.display());
    println!("cargo:rustc-link-lib=static=ra8_box");
}
