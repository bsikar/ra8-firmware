// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Build the C command-line policy used by the Rust-owned executable.

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn run(command: &mut Command, description: &str) {
    let status = command.status().unwrap_or_else(|error| {
        panic!("cannot run {description}: {error}");
    });
    assert!(status.success(), "{description} failed with {status}");
}

fn main() {
    println!("cargo:rerun-if-changed=../src/firmware_report_cli.c");
    println!("cargo:rerun-if-changed=../src/firmware_report_cli.h");
    if let Ok(directory) = env::var("FIRMWARE_REPORT_C_LIB_DIR") {
        println!("cargo:rustc-link-search=native={directory}");
        println!("cargo:rustc-link-lib=static=firmware_report_c");
        return;
    }

    let output = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo provides OUT_DIR"));
    let source = Path::new("../src/firmware_report_cli.c");
    let object = output.join("firmware_report_cli.o");
    let archive = output.join("libfirmware_report_c.a");
    let compiler = env::var("CC").unwrap_or_else(|_| "cc".to_owned());
    run(
        Command::new(compiler)
            .args(["-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-c"])
            .arg(source)
            .arg("-o")
            .arg(&object),
        "C support compilation",
    );
    let archiver = env::var("AR").unwrap_or_else(|_| "ar".to_owned());
    run(
        Command::new(archiver).arg("crs").arg(&archive).arg(&object),
        "C support archive creation",
    );
    println!("cargo:rustc-link-search=native={}", output.display());
    println!("cargo:rustc-link-lib=static=firmware_report_c");
}
