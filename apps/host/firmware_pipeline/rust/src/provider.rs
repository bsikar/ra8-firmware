// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Allocation-free deterministic firmware analysis.

const FNV_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

/// Fixed-layout result returned through the private Rust-to-Zig C membrane.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(C)]
pub struct RustSummary {
    /// Complete input length.
    pub byte_count: u64,
    /// Bytes equal to zero.
    pub zero_count: u64,
    /// Bytes equal to erased flash (`0xff`).
    pub erased_count: u64,
    /// FNV-1a 64-bit change-detection digest.
    pub fnv1a64: u64,
}

/// Analyze one non-empty firmware image.
///
/// Returns `None` for an empty image so integration tests can prove a
/// Rust-originating error crosses both ABI membranes without publishing output.
#[must_use]
pub fn analyze(image: &[u8]) -> Option<RustSummary> {
    if image.is_empty() {
        return None;
    }
    let mut result = RustSummary {
        byte_count: image.len() as u64,
        zero_count: 0,
        erased_count: 0,
        fnv1a64: FNV_OFFSET_BASIS,
    };
    for byte in image {
        result.zero_count += u64::from(*byte == 0);
        result.erased_count += u64::from(*byte == u8::MAX);
        result.fnv1a64 ^= u64::from(*byte);
        result.fnv1a64 = result.fnv1a64.wrapping_mul(FNV_PRIME);
    }
    Some(result)
}
