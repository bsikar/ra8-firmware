// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Idiomatic firmware analysis behind the application's C ABI adapter.

mod foreign;
mod provider;

pub use provider::{ReportSummary, summarize};

/// Keep every public C ABI lifecycle export reachable from a Rust-owned executable.
pub fn retain_foreign_exports() {
    std::hint::black_box(foreign::firmware_report_create as usize);
    std::hint::black_box(foreign::firmware_report_query as usize);
    std::hint::black_box(foreign::firmware_report_release as usize);
}
