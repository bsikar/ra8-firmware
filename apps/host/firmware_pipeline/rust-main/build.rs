// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Build the C support and Zig adapter linked by the Rust-owned executable.

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
    for source in [
        "../src/firmware_pipeline_cli.c",
        "../src/firmware_pipeline_cli.h",
        "../src/firmware_pipeline_io.c",
        "../src/firmware_pipeline_io.h",
        "../zig/src/adapter.zig",
        "../zig/build.zig",
    ] {
        println!("cargo:rerun-if-changed={source}");
    }
    if let (Ok(c_directory), Ok(zig_directory)) = (
        env::var("FIRMWARE_PIPELINE_C_LIB_DIR"),
        env::var("FIRMWARE_PIPELINE_ZIG_LIB_DIR"),
    ) {
        emit_links(Path::new(&c_directory), Path::new(&zig_directory));
        return;
    }

    let output = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo provides OUT_DIR"));
    let c_object_cli = output.join("firmware_pipeline_cli.o");
    let c_object_io = output.join("firmware_pipeline_io.o");
    let c_archive = output.join("libfirmware_pipeline_c.a");
    let compiler = env::var("CC").unwrap_or_else(|_| "cc".to_owned());
    for (source, object) in [
        ("../src/firmware_pipeline_cli.c", &c_object_cli),
        ("../src/firmware_pipeline_io.c", &c_object_io),
    ] {
        run(
            Command::new(&compiler)
                .args([
                    "-std=gnu2x",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-c",
                    source,
                    "-o",
                ])
                .arg(object),
            "C support compilation",
        );
    }
    let archiver = env::var("AR").unwrap_or_else(|_| "ar".to_owned());
    run(
        Command::new(archiver)
            .arg("crs")
            .arg(&c_archive)
            .args([&c_object_cli, &c_object_io]),
        "C support archive creation",
    );

    let zig_prefix = output.join("zig-out");
    let zig = env::var("ZIG").unwrap_or_else(|_| "zig".to_owned());
    run(
        Command::new(zig)
            .current_dir("../zig")
            .args(["build", "library", "--prefix"])
            .arg(&zig_prefix),
        "Zig adapter build",
    );
    emit_links(&output, &zig_prefix.join("lib"));
}

fn emit_links(c_directory: &Path, zig_directory: &Path) {
    println!("cargo:rustc-link-search=native={}", c_directory.display());
    println!("cargo:rustc-link-search=native={}", zig_directory.display());
    println!("cargo:rustc-link-lib=static=firmware_pipeline_c");
    println!("cargo:rustc-link-lib=static=firmware_pipeline_zig");
}
