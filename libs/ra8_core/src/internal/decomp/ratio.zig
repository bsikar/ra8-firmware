//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The bomb bound: `out <= in * max_ratio + grace`, computed so a hostile
//! 64-bit input cannot wrap it.

const std = @import("std");

const policy = @import("decomp_policy");

/// The value a saturated bound reports, at which point only the absolute
/// output cap governs.
pub const saturated: u64 = std.math.maxInt(u64);

/// The largest output byte count the ratio bound admits for `in_total`.
///
/// Both the product and the sum saturate rather than wrapping, which
/// deliberately disables the ratio test for an astronomically large honest
/// input: the output cap still governs that case, and it is the bound that
/// means something there.
pub fn bound(limits: policy.Limits, in_total: u64) u64 {
    const product = std.math.mul(u64, in_total, limits.max_ratio) catch return saturated;
    return std.math.add(u64, product, limits.ratio_grace_bytes) catch saturated;
}
