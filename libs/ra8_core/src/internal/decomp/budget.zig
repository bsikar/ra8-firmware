//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The running budget a decoder charges as it works. One budget tracks one
//! decode unit (streaming decoders) or one archive enumeration (walkers).
//!
//! Layout is the C `ra8_decomp_budget_t`, since decoders own one and pass
//! its address across the ABI.

const std = @import("std");

const policy = @import("decomp_policy");
const ratio = @import("decomp_ratio");

/// Which bound a charge crossed. Each maps to one `k_ra8_err_decomp_*`.
pub const Breach = error{
    OutputCap,
    Ratio,
    Entries,
    Iterations,
    Depth,
};

pub const Budget = extern struct {
    limits: policy.Limits = .{},
    out_bytes: u64 = 0,
    in_bytes: u64 = 0,
    entries: u32 = 0,
    iters: u32 = 0,
    depth: u8 = 0,

    /// Charge produced bytes against the output cap and the ratio bound.
    ///
    /// The accumulator saturates: a delta large enough to wrap it is itself
    /// a breach, so clamping and then testing reports the breach rather
    /// than letting the sum come back under the cap.
    pub fn chargeOutput(self: *Budget, in_total: u64, out_delta: u64) Breach!void {
        self.out_bytes = std.math.add(u64, self.out_bytes, out_delta) catch ratio.saturated;
        self.in_bytes = in_total;
        if (self.out_bytes > self.limits.max_output_bytes) return Breach.OutputCap;
        if (self.out_bytes > ratio.bound(self.limits, in_total)) return Breach.Ratio;
    }

    /// Charge one enumerated archive member against the entry cap.
    pub fn chargeEntry(self: *Budget) Breach!void {
        if (self.entries >= self.limits.max_entries) return Breach.Entries;
        self.entries += 1;
    }

    /// Charge one decode-loop turn against the iteration budget.
    pub fn chargeIter(self: *Budget) Breach!void {
        if (self.iters >= self.limits.max_iterations) return Breach.Iterations;
        self.iters += 1;
    }

    /// Enter one stacked decode layer.
    pub fn enter(self: *Budget) Breach!void {
        if (self.depth >= self.limits.max_depth) return Breach.Depth;
        self.depth += 1;
    }

    /// Leave one stacked decode layer. An unbalanced leave is ignored
    /// rather than wrapped: teardown paths call it unconditionally.
    pub fn leave(self: *Budget) void {
        if (self.depth == 0) return;
        self.depth -= 1;
    }
};

/// Check a member's header-declared sizes before any decoding starts.
pub fn checkDeclared(limits: policy.Limits, comp_size: u64, out_size: u64) Breach!void {
    if (out_size > limits.max_output_bytes) return Breach.OutputCap;
    if (out_size > ratio.bound(limits, comp_size)) return Breach.Ratio;
}
