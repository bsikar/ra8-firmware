// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Rust-owned three-language pipeline command behavior.

mod foreign;

use std::ffi::OsString;

/// Execute the three-language command and return its complete standard output.
///
/// # Errors
///
/// Returns a stable diagnostic key for argument, input, or provider failure.
pub fn run(arguments: &[OsString]) -> Result<String, &'static str> {
    firmware_pipeline_rust::retain_foreign_exports();
    foreign::OwnedImage::read(arguments)?.analyze()
}
