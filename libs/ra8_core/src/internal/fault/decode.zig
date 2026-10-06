//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Symbolic decoding for the Configurable Fault Status Register (CFSR).
//!
//! Cause bits and address-validity bits are deliberately separate. A validity
//! bit says whether MMFAR or BFAR can be trusted; it is not itself a fault
//! cause. The exception reporter walks the static cause table in architectural
//! bit order and emits every asserted cause without allocating in fault context.

/// One architectural CFSR cause bit and the stable text written to the log.
pub const Cause = struct {
    mask: u32,
    message: [:0]const u8,

    pub fn asserted(self: Cause, cfsr: u32) bool {
        return cfsr & self.mask != 0;
    }
};

/// CFSR field bits that qualify an accompanying fault-address register.
pub const valid = struct {
    pub const mmfar: u32 = 1 << 7;
    pub const bfar: u32 = 1 << 15;
};

/// Every CFSR cause bit, ordered from MMFSR through BFSR to UFSR.
///
/// MMARVALID and BFARVALID are intentionally absent: they qualify addresses
/// returned by `mmFaultAddress` and `busFaultAddress` instead of naming causes.
pub const causes = [_]Cause{
    .{ .mask = 1 << 0, .message = "cause=IACCVIOL" },
    .{ .mask = 1 << 1, .message = "cause=DACCVIOL" },
    .{ .mask = 1 << 3, .message = "cause=MUNSTKERR" },
    .{ .mask = 1 << 4, .message = "cause=MSTKERR" },
    .{ .mask = 1 << 5, .message = "cause=MLSPERR" },
    .{ .mask = 1 << 8, .message = "cause=IBUSERR" },
    .{ .mask = 1 << 9, .message = "cause=PRECISERR" },
    .{ .mask = 1 << 10, .message = "cause=IMPRECISERR" },
    .{ .mask = 1 << 11, .message = "cause=UNSTKERR" },
    .{ .mask = 1 << 12, .message = "cause=STKERR" },
    .{ .mask = 1 << 13, .message = "cause=LSPERR" },
    .{ .mask = 1 << 16, .message = "cause=UNDEFINSTR" },
    .{ .mask = 1 << 17, .message = "cause=INVSTATE" },
    .{ .mask = 1 << 18, .message = "cause=INVPC" },
    .{ .mask = 1 << 19, .message = "cause=NOCP" },
    .{ .mask = 1 << 20, .message = "cause=STKOF" },
    .{ .mask = 1 << 24, .message = "cause=UNALIGNED" },
    .{ .mask = 1 << 25, .message = "cause=DIVBYZERO" },
};

/// Return MMFAR only when CFSR says the register is valid for this fault.
pub fn mmFaultAddress(cfsr: u32, mmfar: u32) ?u32 {
    return if (cfsr & valid.mmfar != 0) mmfar else null;
}

/// Return BFAR only when CFSR says the register is valid for this fault.
pub fn busFaultAddress(cfsr: u32, bfar: u32) ?u32 {
    return if (cfsr & valid.bfar != 0) bfar else null;
}
