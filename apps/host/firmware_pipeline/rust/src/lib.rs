// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Safe firmware analysis and its narrow audited C ABI adapter.

#[cfg(feature = "command")]
mod command_ffi;
mod foreign;
mod provider;

#[cfg(feature = "command")]
use std::ffi::OsString;

pub use provider::{RustSummary, analyze};

/// Keep the private C ABI provider reachable in a Rust-owned final executable.
///
/// The Zig archive calls this symbol by its C name, which is invisible to
/// Rust's native reachability analysis unless the final Rust program anchors it.
pub fn retain_foreign_exports() {
    std::hint::black_box(foreign::firmware_pipeline_rust_analyze as usize);
}

/// Execute the three-language command and return its complete standard output.
///
/// # Errors
///
/// Returns a stable diagnostic key for argument, input, or provider failure.
#[cfg(feature = "command")]
pub fn run(arguments: &[OsString]) -> Result<String, &'static str> {
    retain_foreign_exports();
    command_ffi::OwnedImage::read(arguments)?.analyze()
}
