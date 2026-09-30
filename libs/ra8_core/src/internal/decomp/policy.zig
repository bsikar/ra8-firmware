//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The decompression policy record: the five resource bounds every archive
//! and stream decoder enforces, and whether a given policy is usable.
//!
//! Layout is the C `ra8_decomp_limits_t` from `inc/ra8_decomp_limits.h`,
//! since decoders pass one across the ABI by value.

/// Owner-approved defaults, the C `ra8_decomp_defaults_t` values.
pub const defaults = struct {
    /// Per-decode-unit output cap: the SDRAM working-set ceiling, 64 MiB.
    pub const output_bytes: u64 = 64 * 1024 * 1024;
    /// Output:input ratio bound. Above DEFLATE's ~1032:1 per-layer maximum
    /// lies only bomb territory.
    pub const max_ratio: u32 = 1024;
    /// Additive grace, 64 KiB, so a small honest file's fixed headers do
    /// not trip a pure quotient.
    pub const ratio_grace: u32 = 64 * 1024;
    /// Per-archive entry cap, an order of magnitude above the largest real
    /// comic volume or EPUB spine.
    pub const max_entries: u32 = 4096;
    /// Loop iteration budget, 1 Mi: the P10 Rule 2 backstop.
    pub const max_iters: u32 = 1048576;
    /// Stacked decode-layer cap: wrapper stream plus inner container.
    pub const max_depth: u8 = 2;
};

/// One policy. Value type, copied freely.
pub const Limits = extern struct {
    max_output_bytes: u64 = 0,
    max_ratio: u32 = 0,
    ratio_grace_bytes: u32 = 0,
    max_entries: u32 = 0,
    max_iterations: u32 = 0,
    max_depth: u8 = 0,

    /// Whether every bound is non-zero and so usable. A zero cap is always
    /// a configuration bug: it would reject all input, and a zero grace is
    /// ambiguous rather than permissive.
    pub fn usable(self: Limits) bool {
        if (self.max_output_bytes == 0) return false;
        if (self.max_ratio == 0) return false;
        if (self.ratio_grace_bytes == 0) return false;
        if (self.max_entries == 0) return false;
        if (self.max_iterations == 0) return false;
        return self.max_depth != 0;
    }
};

/// The one policy every production decoder runs under.
pub fn default() Limits {
    return .{
        .max_output_bytes = defaults.output_bytes,
        .max_ratio = defaults.max_ratio,
        .ratio_grace_bytes = defaults.ratio_grace,
        .max_entries = defaults.max_entries,
        .max_iterations = defaults.max_iters,
        .max_depth = defaults.max_depth,
    };
}
