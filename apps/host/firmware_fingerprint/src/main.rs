// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

use std::ffi::OsString;
use std::fs;
use std::io;

fn main() {
    let args: Vec<OsString> = std::env::args_os().skip(1).collect();
    let mut stdout = io::stdout().lock();
    let mut stderr = io::stderr().lock();
    let status = firmware_fingerprint::run(args, |path| fs::read(path), &mut stdout, &mut stderr);
    std::process::exit(status);
}
