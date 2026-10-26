//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the RSIP device-security logic (RA8FW-1064). The Hw below
//! is a fake register file over the six invented "security-state" offsets.
//! The fail-closed contract (guarded=false refuses every entry point and
//! writes no fabricated state) is the test that matters most: a security
//! query must refuse, not lie.

const std = @import("std");
const devsec = @import("rsip_devsec");

const Fake = struct {
    const zero: [64]u32 = @splat(0);
    regs: [64]u32 = zero,

    const Hw = struct {
        f: *Fake,
        pub fn read32(h: Hw, off: usize) u32 {
            return h.f.regs[off >> 2];
        }
        pub fn write32(h: Hw, off: usize, value: u32) void {
            h.f.regs[off >> 2] = value;
        }
    };

    fn hw(self: *Fake) Hw {
        return .{ .f = self };
    }
};

const sentinel32: u32 = 0x5A5A_5A5A;
const sentinel8: u8 = 0xA5;

test "fail-closed: every entry point refuses and writes no state" {
    var f = Fake{};
    var out32: u32 = sentinel32;
    var out8: u8 = sentinel8;

    try std.testing.expectEqual(devsec.not_supported, devsec.lifeGet(f.hw(), false, &out32));
    try std.testing.expectEqual(sentinel32, out32);
    try std.testing.expectEqual(devsec.not_supported, devsec.lifeAdvance(f.hw(), false, 0));
    try std.testing.expectEqual(devsec.not_supported, devsec.debugLevelGet(f.hw(), false, &out8));
    try std.testing.expectEqual(@as(u8, sentinel8), out8);
    try std.testing.expectEqual(devsec.not_supported, devsec.debugLevelSet(f.hw(), false, 0));
    try std.testing.expectEqual(devsec.not_supported, devsec.tamperEnable(f.hw(), false, 1));
    try std.testing.expectEqual(devsec.not_supported, devsec.tamperStatus(f.hw(), false, &out32));
    try std.testing.expectEqual(sentinel32, out32);
    try std.testing.expectEqual(devsec.not_supported, devsec.tamperAck(f.hw(), false, 1));
    try std.testing.expectEqual(devsec.not_supported, devsec.dpaArm(f.hw(), false, true));
    try std.testing.expectEqual(Fake.zero, f.regs);
}

test "guarded: lifecycle advances monotonically toward rma" {
    var f = Fake{};
    var out: u32 = 0;
    try std.testing.expectEqual(devsec.ok, devsec.lifeGet(f.hw(), true, &out));
    try std.testing.expectEqual(@as(u32, 0), out);

    try std.testing.expectEqual(devsec.ok, devsec.lifeAdvance(f.hw(), true, devsec.life_rma));
    try std.testing.expectEqual(devsec.ok, devsec.lifeGet(f.hw(), true, &out));
    try std.testing.expectEqual(devsec.life_rma, out);

    // RMA is terminal: anything below the current state is invalid_state,
    // and nothing above it exists.
    try std.testing.expectEqual(devsec.invalid_state, devsec.lifeAdvance(f.hw(), true, 0));
    try std.testing.expectEqual(devsec.invalid_arg, devsec.lifeAdvance(f.hw(), true, devsec.life_rma + 1));
    try std.testing.expectEqual(devsec.life_rma, f.regs[devsec.off_life_state >> 2]);
}

test "guarded: debug level bounds at al2" {
    var f = Fake{};
    var out: u8 = 0;
    try std.testing.expectEqual(devsec.ok, devsec.debugLevelSet(f.hw(), true, devsec.debug_al2));
    try std.testing.expectEqual(devsec.ok, devsec.debugLevelGet(f.hw(), true, &out));
    try std.testing.expectEqual(devsec.debug_al2, out);
    try std.testing.expectEqual(devsec.invalid_arg, devsec.debugLevelSet(f.hw(), true, devsec.debug_al2 + 1));
    try std.testing.expectEqual(devsec.debug_al2, f.regs[devsec.off_debug_level >> 2]);
}

test "guarded: tamper sources stay inside the mask" {
    var f = Fake{};
    try std.testing.expectEqual(devsec.ok, devsec.tamperEnable(f.hw(), true, devsec.tamper_src_all));
    try std.testing.expectEqual(devsec.tamper_src_all, f.regs[devsec.off_tamper_ctrl >> 2]);
    try std.testing.expectEqual(devsec.invalid_arg, devsec.tamperEnable(f.hw(), true, devsec.tamper_src_all | 0x8000_0000));

    try std.testing.expectEqual(devsec.invalid_arg, devsec.tamperAck(f.hw(), true, 0));
    try std.testing.expectEqual(devsec.invalid_arg, devsec.tamperAck(f.hw(), true, devsec.tamper_src_all << 1));
    try std.testing.expectEqual(devsec.ok, devsec.tamperAck(f.hw(), true, 0x0003));
    try std.testing.expectEqual(@as(u32, 0x0003), f.regs[devsec.off_tamper_status >> 2]);
}

test "guarded: dpa arm is a read-modify-write of CTRL" {
    var f = Fake{};
    f.regs[devsec.off_ctrl >> 2] = 0xFFFF_FFF7; // DPA_ARM clear, rest set
    try std.testing.expectEqual(devsec.ok, devsec.dpaArm(f.hw(), true, true));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), f.regs[devsec.off_ctrl >> 2]);
    try std.testing.expectEqual(@as(u32, 1), f.regs[devsec.off_dpa_ctrl >> 2]);

    try std.testing.expectEqual(devsec.ok, devsec.dpaArm(f.hw(), true, false));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFF7), f.regs[devsec.off_ctrl >> 2]);
    try std.testing.expectEqual(@as(u32, 0), f.regs[devsec.off_dpa_ctrl >> 2]);
}
