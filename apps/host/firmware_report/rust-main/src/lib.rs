// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Rust-owned firmware report command behavior.

mod foreign;

use std::ffi::OsString;
use std::fmt::Write as _;
use std::fs::File;
use std::io::Read as _;

const MAX_IMAGE_SIZE: usize = 16 * 1024 * 1024;

/// Execute the command and return its complete standard output.
///
/// # Errors
///
/// Returns a stable diagnostic key for argument, input, or formatting failure.
pub fn run(arguments: &[OsString]) -> Result<String, &'static str> {
    let path = foreign::parse_path(arguments).map_err(|()| "usage")?;
    let file = File::open(path).map_err(|_| "cannot open input")?;
    let mut image = Vec::new();
    file.take((MAX_IMAGE_SIZE + 1) as u64)
        .read_to_end(&mut image)
        .map_err(|_| "cannot open input")?;
    if image.len() > MAX_IMAGE_SIZE {
        return Err("input exceeds the 16 MiB limit");
    }
    let summary = foreign::summarize(&image).map_err(|()| "language pipeline failed")?;
    let mut output = String::new();
    writeln!(output, "bytes={}", summary.byte_count).map_err(|_| "cannot write output")?;
    writeln!(output, "zero={}", summary.zero_count).map_err(|_| "cannot write output")?;
    writeln!(output, "erased={}", summary.erased_count).map_err(|_| "cannot write output")?;
    writeln!(output, "fnv1a64={:016x}", summary.fnv1a64).map_err(|_| "cannot write output")?;
    Ok(output)
}
