// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Safe implementation behind the fixture's narrow C adapter.

/// Stable result values shared with the hand-authored C header.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u16)]
pub enum AbiResult {
    /// Success.
    Ok = 0,
    /// The bounded handle slot is unavailable.
    NoMemory = 0x102,
    /// An argument or handle is invalid.
    InvalidArgument = 0x103,
    /// Arithmetic overflowed the output width.
    InvalidSize = 0x105,
    /// A required pointer is null.
    NullPointer = 0x504,
}

/// Fixed-layout input shared with C23.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(C)]
pub struct AbiConfig {
    /// Value to scale.
    pub value: u32,
    /// Unsigned scale factor.
    pub factor: u16,
    /// Canonical byte boolean.
    pub enabled: u8,
    /// Reserved and required to be zero.
    pub reserved0: u8,
}

/// Validate and evaluate a configuration without touching foreign pointers.
///
/// # Errors
///
/// Returns [`AbiResult::InvalidArgument`] for a non-canonical field or
/// [`AbiResult::InvalidSize`] when multiplication overflows.
pub fn apply_config(config: AbiConfig) -> Result<u32, AbiResult> {
    if config.enabled > 1 || config.reserved0 != 0 {
        return Err(AbiResult::InvalidArgument);
    }
    if config.enabled == 0 {
        return Ok(config.value);
    }
    config
        .value
        .checked_mul(u32::from(config.factor))
        .ok_or(AbiResult::InvalidSize)
}
