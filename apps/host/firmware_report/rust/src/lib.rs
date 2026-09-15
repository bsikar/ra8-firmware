// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Idiomatic firmware analysis behind the application's C ABI adapter.

mod foreign;
mod provider;

pub use provider::{ReportSummary, summarize};
