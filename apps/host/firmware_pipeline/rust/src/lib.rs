// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Safe firmware analysis and its narrow audited C ABI adapter.

mod foreign;
mod provider;

pub use provider::{RustSummary, analyze};
