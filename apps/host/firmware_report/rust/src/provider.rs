// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Safe, allocation-free firmware image analysis.

const FNV_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

/// Stable values returned through the public C representation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(C)]
pub struct ReportSummary {
    /// Complete input size.
    pub byte_count: u64,
    /// Bytes equal to zero.
    pub zero_count: u64,
    /// Bytes equal to the erased-flash value `0xff`.
    pub erased_count: u64,
    /// FNV-1a 64-bit digest of the complete image.
    pub fnv1a64: u64,
}

/// Analyze a complete firmware image deterministically.
#[must_use]
pub fn summarize(image: &[u8]) -> ReportSummary {
    let mut summary = ReportSummary {
        byte_count: image.len() as u64,
        zero_count: 0,
        erased_count: 0,
        fnv1a64: FNV_OFFSET_BASIS,
    };
    for byte in image {
        summary.zero_count += u64::from(*byte == 0);
        summary.erased_count += u64::from(*byte == u8::MAX);
        summary.fnv1a64 ^= u64::from(*byte);
        summary.fnv1a64 = summary.fnv1a64.wrapping_mul(FNV_PRIME);
    }
    summary
}
