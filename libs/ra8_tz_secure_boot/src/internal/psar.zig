//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The peripheral security attribution gate, decided before it is driven.
//!
//! Opening PRC4 and spinning on a read-back is MMIO the ABI layer performs;
//! what a given address and mask actually ask for is decided here.

const regs = @import("regs.zig");

/// What a caller's request amounts to.
pub const Plan = union(enum) {
    /// The address is not a register; refuse before touching the gate.
    reject: regs.ErrCode,
    /// An empty mask asks for nothing: report success and the current value
    /// without ever opening the gate.
    observe,
    /// OR `bits` into the register behind the protection gate.
    apply: u32,
};

/// Decide what a `set_ns` request should do.
pub fn plan(addr: usize, ns_mask: u32) Plan {
    if (addr == 0) return .{ .reject = regs.Err.invalid_arg };
    if (ns_mask == 0) return .observe;
    return .{ .apply = ns_mask };
}

/// The value a masked write should leave behind. Bits already handed to the
/// Non-Secure world stay handed over; the gate only ever opens access.
pub fn merged(current: u32, ns_mask: u32) u32 {
    return current | ns_mask;
}

/// Whether a read-back shows the write landed.
pub fn settled(seen: u32, ns_mask: u32) bool {
    return seen & ns_mask == ns_mask;
}
