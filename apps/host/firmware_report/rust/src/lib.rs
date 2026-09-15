// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Idiomatic firmware analysis behind the application's C ABI adapter.

#[cfg(feature = "command")]
mod command_ffi;
mod foreign;
mod provider;

#[cfg(feature = "command")]
use std::ffi::OsString;
#[cfg(feature = "command")]
use std::fmt::Write as _;
#[cfg(feature = "command")]
use std::fs::File;
#[cfg(feature = "command")]
use std::io::Read as _;

#[cfg(feature = "command")]
const MAX_IMAGE_SIZE: usize = 16 * 1024 * 1024;

pub use provider::{ReportSummary, summarize};

/// Keep every public C ABI lifecycle export reachable from a Rust-owned executable.
pub fn retain_foreign_exports() {
    std::hint::black_box(foreign::firmware_report_create as usize);
    std::hint::black_box(foreign::firmware_report_query as usize);
    std::hint::black_box(foreign::firmware_report_release as usize);
}

/// Execute the command and return its complete standard output.
///
/// # Errors
///
/// Returns a stable diagnostic key for argument, input, or formatting failure.
#[cfg(feature = "command")]
pub fn run(arguments: &[OsString]) -> Result<String, &'static str> {
    let path = command_ffi::parse_path(arguments).map_err(|()| "usage")?;
    let file = File::open(path).map_err(|_| "cannot open input")?;
    let mut image = Vec::new();
    file.take((MAX_IMAGE_SIZE + 1) as u64)
        .read_to_end(&mut image)
        .map_err(|_| "cannot open input")?;
    if image.len() > MAX_IMAGE_SIZE {
        return Err("input exceeds the 16 MiB limit");
    }
    let summary = command_ffi::summarize(&image).map_err(|()| "language pipeline failed")?;
    let mut output = String::new();
    writeln!(output, "bytes={}", summary.byte_count).map_err(|_| "cannot write output")?;
    writeln!(output, "zero={}", summary.zero_count).map_err(|_| "cannot write output")?;
    writeln!(output, "erased={}", summary.erased_count).map_err(|_| "cannot write output")?;
    writeln!(output, "fnv1a64={:016x}", summary.fnv1a64).map_err(|_| "cannot write output")?;
    Ok(output)
}
