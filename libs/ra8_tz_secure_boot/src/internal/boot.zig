//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The secure-boot progress counter and the host-side register capture.
//!
//! The sequencer's MMIO stores live in the ABI layer; what lives here is the
//! step a bench probe reads back, and the capture an off-target build keeps
//! in place of the stores so unit tests can assert what would have landed.

const std = @import("std");
const regs = @import("regs.zig");

/// Progress markers stamped as the boot makes forward progress. The values
/// are the published contract a J-Link memprobe indexes against.
pub const Step = enum(u8) {
    idle = 0,
    sau_done = 1,
    prcr_unlocked = 2,
    ipcsar_written = 3,
    prcr_relocked = 4,
    blxns_armed = 5,
    branched = 6,
};

/// What an off-target build records instead of writing silicon.
pub const Host = struct {
    prcr_s_last: u16 = 0,
    prcr_unlock_count: u8 = 0,
    prcr_relock_count: u8 = 0,
    ipcsar_value: u32 = 0,
    ipcpar_value: u32 = 0,
    blxns_target: u32 = 0,
    blxns_msp_ns: u32 = 0,
    vtor_ns: u32 = 0,

    /// Record a 32-bit store. Addresses outside the documented set are
    /// ignored: no other 32-bit register is written through this path.
    pub fn write32(self: *Host, addr: usize, value: u32) void {
        switch (addr) {
            regs.Addr.ipcsar => self.ipcsar_value = value,
            regs.Addr.ipcpar => self.ipcpar_value = value,
            regs.Addr.vtor_ns => self.vtor_ns = value,
            else => {},
        }
    }

    /// Record a 16-bit store. PRCR_S is the only 16-bit writer, so the PRC4
    /// bit decides whether this opened or closed the gate.
    pub fn write16(self: *Host, value: u16) void {
        self.prcr_s_last = value;
        if (value & regs.Prcr.prc4 != 0) {
            self.prcr_unlock_count +%= 1;
        } else {
            self.prcr_relock_count +%= 1;
        }
    }

    /// True while every gate this run opened has been closed again.
    pub fn balanced(self: Host) bool {
        return self.prcr_unlock_count == self.prcr_relock_count;
    }
};

/// Live progress counter, readable at any point in the sequence.
pub var step: Step = .idle;

/// Live host capture; untouched on a target build.
pub var host: Host = .{};

/// Return both to their power-on values so a test starts from a known state.
pub fn reset() void {
    step = .idle;
    host = .{};
}

test "a full gate cycle leaves the capture balanced" {
    reset();
    host.write16(regs.Prcr.open);
    host.write32(regs.Addr.ipcsar, 0xABCD);
    host.write32(regs.Addr.ipcpar, 0x1234);
    host.write16(regs.Prcr.close);
    try std.testing.expectEqual(@as(u32, 0xABCD), host.ipcsar_value);
    try std.testing.expectEqual(@as(u32, 0x1234), host.ipcpar_value);
    try std.testing.expectEqual(@as(u8, 1), host.prcr_unlock_count);
    try std.testing.expect(host.balanced());
    try std.testing.expectEqual(regs.Prcr.close, host.prcr_s_last);
}

test "an unrelated address changes nothing" {
    reset();
    host.write32(0x4000_0000, 0xDEAD);
    try std.testing.expectEqual(@as(u32, 0), host.ipcsar_value);
    try std.testing.expectEqual(@as(u32, 0), host.vtor_ns);
}

test "reset clears a dirtied capture" {
    reset();
    host.write16(regs.Prcr.open);
    host.vtor_ns = 0x2008_0000;
    step = .blxns_armed;
    reset();
    try std.testing.expectEqual(Step.idle, step);
    try std.testing.expectEqual(@as(u32, 0), host.vtor_ns);
    try std.testing.expect(host.balanced());
}
