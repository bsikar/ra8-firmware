//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dotf_ctrl.zig.

const std = @import("std");
const ctrl = @import("dotf_ctrl");
const state = ctrl.state_mod;

const FakeRegs = struct {
    reg00: u32 = 0xFFFF_FFFF,
    writes: u32 = 0,
    pub fn writeReg00(self: *FakeRegs, v: u32) void {
        self.reg00 = v;
        self.writes += 1;
    }
};

fn reset() state.ChanState {
    var st = state.ChanState{};
    state.reset(&st);
    return st;
}

test "REG00 assembles mode, key size, SCA and enable" {
    var st = reset();
    try std.testing.expectEqual(@as(u32, 0x2201_0000), ctrl.reg00(&st, false));
    try std.testing.expectEqual(@as(u32, 0x2201_0200), ctrl.reg00(&st, true));
    st.cached_sca = ctrl.sca_max;
    st.cached_key_size = ctrl.key_size_256;
    try std.testing.expectEqual(@as(u32, 0x2303_0200), ctrl.reg00(&st, true));
    st.cached_sca = ctrl.sca_off;
    try std.testing.expectEqual(@as(u32, 0x2300_0000), ctrl.reg00(&st, false));
}

test "enable then disable" {
    var st = reset();
    var regs = FakeRegs{};
    ctrl.enable(&st, &regs);
    try std.testing.expectEqual(@as(u32, 0x2201_0200), regs.reg00);
    try std.testing.expectEqual(@as(u8, 1), st.enabled);
    ctrl.disable(&st, &regs);
    try std.testing.expectEqual(@as(u32, 0), regs.reg00);
    try std.testing.expectEqual(@as(u8, 0), st.enabled);
}

test "setSca and setKeySize cache always, write only while armed" {
    var st = reset();
    var regs = FakeRegs{};
    try std.testing.expectEqual(ctrl.invalid_arg, ctrl.setSca(&st, &regs, 3));
    try std.testing.expectEqual(ctrl.invalid_arg, ctrl.setKeySize(&st, &regs, 0x0400_0000));
    try std.testing.expectEqual(ctrl.ok, ctrl.setSca(&st, &regs, ctrl.sca_max));
    try std.testing.expectEqual(ctrl.ok, ctrl.setKeySize(&st, &regs, ctrl.key_size_192));
    try std.testing.expectEqual(@as(u32, 0), regs.writes);
    try std.testing.expectEqual(ctrl.key_size_192, st.cached_key_size);
    st.enabled = 1;
    try std.testing.expectEqual(ctrl.ok, ctrl.setSca(&st, &regs, ctrl.sca_off));
    try std.testing.expectEqual(@as(u32, 0x2100_0200), regs.reg00);
    try std.testing.expectEqual(ctrl.ok, ctrl.setKeySize(&st, &regs, ctrl.key_size_128));
    try std.testing.expectEqual(@as(u32, 0x2200_0200), regs.reg00);
    try std.testing.expectEqual(@as(u32, 2), regs.writes);
}
