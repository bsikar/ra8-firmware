// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Rust provider used only to prove the repository's public C ABI contract.

mod provider;

pub use provider::{AbiConfig, AbiResult, apply_config};

mod foreign;
