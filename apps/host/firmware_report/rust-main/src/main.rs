// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Rust entry point for the mixed C-and-Rust firmware report.

use std::io::{self, Write as _};

fn main() {
    let arguments: Vec<std::ffi::OsString> = std::env::args_os().collect();
    match firmware_report::run(&arguments) {
        Ok(output) => {
            if io::stdout().write_all(output.as_bytes()).is_err() {
                eprintln!("firmware_report: cannot write output");
                std::process::exit(2);
            }
        }
        Err("usage") => {
            eprintln!("usage: firmware_report <firmware-image>");
            std::process::exit(2);
        }
        Err(message) => {
            eprintln!("firmware_report: {message}");
            std::process::exit(2);
        }
    }
}
