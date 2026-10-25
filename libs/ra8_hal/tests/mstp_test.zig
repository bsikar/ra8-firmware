//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the MSTP gate logic (RA8FW-770). The Hw below is a fake
//! register file with write-ignore masks (a Non-secure-owned bit ignores a
//! Secure write) and a per-register stuck flag driving the poll seam, so
//! the read-back timeout and rollback legs run deterministically.

const std = @import("std");
const mstp = @import("../src/internal/mstp.zig");

const zero5: [5]u32 = @splat(0);

const Fake = struct {
    regs: [5]u32 = zero5,
    psars: [5]u32 = zero5,
    /// Bits a write cannot change (a Non-secure-owned MSTP bit).
    ignore_mask: [5]u32 = zero5,
    /// Register whose poll never settles (a wedged SYSC bus).
    stuck_reg: ?u8 = null,
    polls: usize = 0,
    errors: usize = 0,

    const Hw = struct {
        f: *Fake,
        pub fn read(h: Hw, reg: u8) u32 {
            return h.f.regs[reg];
        }
        pub fn write(h: Hw, reg: u8, value: u32) void {
            const m = h.f.ignore_mask[reg];
            h.f.regs[reg] = (h.f.regs[reg] & m) | (value & ~m);
        }
        pub fn psar(h: Hw, reg: u8) u32 {
            return h.f.psars[reg];
        }
        pub fn poll(h: Hw, reg: u8, iter: u32, cond: bool) bool {
            _ = iter;
            h.f.polls += 1;
            if (h.f.stuck_reg != null and h.f.stuck_reg.? == reg) return false;
            return cond;
        }
        pub fn info(_: Hw, _: [*:0]const u8) void {}
        pub fn err(_: Hw, _: [*:0]const u8) void {}
        pub fn errVal(h: Hw, _: [*:0]const u8, _: u32) void {
            h.f.errors += 1;
        }
    };

    fn hw(self: *Fake) Hw {
        return .{ .f = self };
    }
};

/// k_ra8_mstp_usbfs: MSTPCRB bit 11 (USBFS0).
const usbfs: u16 = (1 << 8) | 11;
const usbfs_bit: u32 = 1 << 11;

test "init gates every module and zeroes the refcounts" {
    var f = Fake{};
    var gate = mstp.Gate{};
    gate.counts[2][4] = 9; // stale counts must be cleared
    try std.testing.expectEqual(mstp.k_ra8_ok, mstp.init(f.hw(), &gate));
    try std.testing.expectEqual(mstp.safe_stopped_a, f.regs[0]);
    for (1..5) |r| try std.testing.expectEqual(mstp.all_stopped, f.regs[r]);
    for (0..5) |r| {
        for (0..32) |b| {
            try std.testing.expectEqual(@as(u8, 0), gate.counts[r][b]);
        }
    }
}

test "init read-back tolerates Non-secure-owned bits" {
    var f = Fake{};
    f.psars[1] = usbfs_bit; // USBFS0 delegated Non-secure
    f.ignore_mask[1] = usbfs_bit; // ...so MSTPB11 ignores the Secure write
    var gate = mstp.Gate{};
    try std.testing.expectEqual(mstp.k_ra8_ok, mstp.init(f.hw(), &gate));
    // The NS-owned bit stayed 0 (the write was ignored) and did not hold
    // the read-back open.
    try std.testing.expectEqual(@as(u32, 0), f.regs[1] & usbfs_bit);
    // Without the mask this register would never settle: the strict path
    // times out on the same fake.
    var strict = Fake{};
    var gate2 = mstp.Gate{};
    try std.testing.expectEqual(
        mstp.k_ra8_err_hw_timeout,
        mstp.waitRegSettle(strict.hw(), 1, mstp.all_stopped, ~@as(u32, 0)),
    );
    // nsMask reads the PSAR for B..E and 0 for A (no PSAR word).
    try std.testing.expectEqual(usbfs_bit, mstp.nsMask(f.hw(), 1));
    try std.testing.expectEqual(@as(u32, 0), mstp.nsMask(f.hw(), 0));
}

test "enable and disable only move the register on the count edges" {
    var f = Fake{};
    var gate = mstp.Gate{};
    try std.testing.expectEqual(mstp.k_ra8_ok, mstp.enable(f.hw(), &gate, usbfs));
    try std.testing.expectEqual(@as(u32, 0), f.regs[1] & usbfs_bit);
    try std.testing.expectEqual(mstp.k_ra8_ok, mstp.enable(f.hw(), &gate, usbfs));
    try std.testing.expectEqual(@as(u32, 0), f.regs[1] & usbfs_bit);
    var ref: u8 = 0;
    try std.testing.expectEqual(mstp.k_ra8_ok, mstp.refcount(f.hw(), &gate, usbfs, &ref));
    try std.testing.expectEqual(@as(u8, 2), ref);
    try std.testing.expectEqual(mstp.k_ra8_ok, mstp.disable(f.hw(), &gate, usbfs));
    try std.testing.expectEqual(@as(u32, 0), f.regs[1] & usbfs_bit);
    try std.testing.expectEqual(mstp.k_ra8_ok, mstp.disable(f.hw(), &gate, usbfs));
    try std.testing.expectEqual(usbfs_bit, f.regs[1] & usbfs_bit);
    try std.testing.expectEqual(mstp.k_ra8_err_invalid_state, mstp.disable(f.hw(), &gate, usbfs));
    // The bit-level read-backs ran through the poll seam.
    try std.testing.expect(f.polls >= 4);
    var stopped_v: bool = false;
    try std.testing.expectEqual(mstp.k_ra8_ok, mstp.stopped(f.hw(), &gate, usbfs, &stopped_v));
    try std.testing.expectEqual(true, stopped_v);
}

test "refcount saturates and underflows" {
    var f = Fake{};
    var gate = mstp.Gate{};
    gate.counts[1][11] = std.math.maxInt(u8);
    try std.testing.expectEqual(mstp.k_ra8_err_invalid_state, mstp.enable(f.hw(), &gate, usbfs));
    try std.testing.expectEqual(@as(u8, std.math.maxInt(u8)), gate.counts[1][11]);
    gate.counts[1][11] = 0;
    try std.testing.expectEqual(mstp.k_ra8_err_invalid_state, mstp.disable(f.hw(), &gate, usbfs));
    try std.testing.expectEqual(@as(u8, 0), gate.counts[1][11]);
}

test "read-back timeout rolls the refcount back" {
    var f = Fake{};
    f.stuck_reg = 1;
    var gate = mstp.Gate{};
    try std.testing.expectEqual(mstp.k_ra8_err_hw_timeout, mstp.enable(f.hw(), &gate, usbfs));
    try std.testing.expectEqual(@as(u8, 0), gate.counts[1][11]);
    try std.testing.expectEqual(mstp.readback_spin, f.polls);
}

test "invalid ids and null outputs error cleanly" {
    var f = Fake{};
    var gate = mstp.Gate{};
    const bad_reg: u16 = 5 << 8;
    const bad_bit: u16 = 32;
    try std.testing.expectEqual(mstp.k_ra8_err_invalid_arg, mstp.enable(f.hw(), &gate, bad_reg));
    try std.testing.expectEqual(mstp.k_ra8_err_invalid_arg, mstp.disable(f.hw(), &gate, bad_bit));
    // Null outputs are checked before the id is decoded.
    try std.testing.expectEqual(mstp.k_ra8_err_null_ptr, mstp.refcount(f.hw(), &gate, usbfs, null));
    try std.testing.expectEqual(mstp.k_ra8_err_null_ptr, mstp.stopped(f.hw(), &gate, usbfs, null));
    var ref: u8 = 0;
    try std.testing.expectEqual(mstp.k_ra8_err_invalid_arg, mstp.refcount(f.hw(), &gate, bad_reg, &ref));
    var stopped_v: bool = false;
    try std.testing.expectEqual(mstp.k_ra8_err_invalid_arg, mstp.stopped(f.hw(), &gate, bad_reg, &stopped_v));
    try std.testing.expect(mstp.decode(bad_bit) == null);
}
