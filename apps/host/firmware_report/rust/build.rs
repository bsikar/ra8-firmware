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
    println!("cargo:rerun-if-changed=../src/firmware_report_cli_internal.h");
    println!("cargo:rerun-if-changed=../../../../libs/ra8_core/inc/ra8_attributes.h");
    if env::var_os("CARGO_FEATURE_COMMAND").is_none() {
        return;
    }
    if let Ok(directory) = env::var("FIRMWARE_REPORT_C_LIB_DIR") {
        emit_links(Path::new(&directory));
        return;
    }

    let output = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo provides OUT_DIR"));
    let source = Path::new("../src/firmware_report_cli.c");
    let object = output.join("firmware_report_cli.o");
    let archive = output.join("libfirmware_report_c.a");
    let compiler = env::var("CC").unwrap_or_else(|_| "cc".to_owned());
    run(
        Command::new(compiler)
            .args([
                "-std=gnu2x",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I../../../../libs/ra8_core/inc",
                "-c",
            ])
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
    emit_links(&output);
}

fn emit_links(directory: &Path) {
    for argument in [
        format!("-L{}", directory.display()),
        "-l:libfirmware_report_c.a".to_owned(),
    ] {
        println!("cargo:rustc-link-arg-bin=firmware_report={argument}");
        println!("cargo:rustc-link-arg-tests={argument}");
    }
}
