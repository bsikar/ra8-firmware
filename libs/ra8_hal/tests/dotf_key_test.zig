//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dotf_key.zig.

const std = @import("std");
const key = @import("dotf_key");
const state = key.state_mod;
const ctrl = key.ctrl_mod;

/// Records REG00 writes as 'Z' (zero) or 'A' (armed) and REG03 words.
const FakeRegs = struct {
    order: [32]u8 = undefined,
    n: usize = 0,
    reg03: [16]u32 = undefined,
    m: usize = 0,
    pub fn writeReg00(self: *FakeRegs, v: u32) void {
        self.order[self.n] = if (v == 0) 'Z' else 'A';
        self.n += 1;
    }
    pub fn writeReg03(self: *FakeRegs, v: u32) void {
        self.order[self.n] = 'w';
        self.n += 1;
        self.reg03[self.m] = v;
        self.m += 1;
    }
    fn seq(self: *const FakeRegs) []const u8 {
        return self.order[0..self.n];
    }
};

fn reset() state.ChanState {
    var st = state.ChanState{};
    state.reset(&st);
    return st;
}

fn handle(size: u32) state.KeyHandle {
    var h = state.KeyHandle{ .size = size, .valid = 1 };
    for (&h.words, 0..) |*w, i| w.* = 0x0102_0300 + @as(u32, @intCast(i));
    return h;
}

test "keyWords and handle checks" {
    try std.testing.expectEqual(@as(u8, 4), key.keyWords(ctrl.key_size_128));
    try std.testing.expectEqual(@as(u8, 6), key.keyWords(ctrl.key_size_192));
    try std.testing.expectEqual(@as(u8, 8), key.keyWords(ctrl.key_size_256));
    var h = handle(ctrl.key_size_128);
    try std.testing.expect(key.validHandle(&h));
    h.valid = 0;
    try std.testing.expect(!key.validHandle(&h));
    h = handle(0x0400_0000);
    try std.testing.expect(!key.validHandle(&h));
}

test "installKey caches and stages byte-swapped words" {
    var st = reset();
    var regs = FakeRegs{};
    var bad = handle(ctrl.key_size_192);
    bad.valid = 0;
    try std.testing.expectEqual(key.invalid_arg, key.installKey(&st, &regs, &bad));
    try std.testing.expectEqual(@as(usize, 0), regs.n);
    const h = handle(ctrl.key_size_192);
    try std.testing.expectEqual(key.ok, key.installKey(&st, &regs, &h));
    try std.testing.expectEqualSlices(u8, "wwwwww", regs.seq());
    try std.testing.expectEqual(@as(u32, 0x0003_0201), regs.reg03[0]);
    try std.testing.expectEqual(@as(u32, 0x0503_0201), regs.reg03[5]);
    try std.testing.expectEqual(ctrl.key_size_192, st.cached_key_size);
    try std.testing.expectEqual(@as(u8, 1), st.key.valid);
}

test "setIv caches and stages four words" {
    var st = reset();
    var regs = FakeRegs{};
    const iv = key.Iv{ 0x1122_3344, 2, 3, 4 };
    key.setIv(&st, &regs, &iv);
    try std.testing.expectEqual(@as(u8, 1), st.iv_valid);
    try std.testing.expectEqual(iv, st.iv_cache);
    try std.testing.expectEqual(@as(u32, 0x4433_2211), regs.reg03[0]);
    try std.testing.expectEqual(@as(usize, 4), regs.m);
}

test "rotate validation" {
    var st = reset();
    const h = handle(ctrl.key_size_128);
    try std.testing.expectEqual(key.invalid_state, key.validateRotate(&st, &h));
    st.active_region_id = 0;
    try std.testing.expectEqual(key.ok, key.validateRotate(&st, &h));
    var bad = h;
    bad.size = 0;
    try std.testing.expectEqual(key.invalid_arg, key.validateRotate(&st, &bad));
}

test "rotate quiesces, restages, and re-arms only if armed" {
    var st = reset();
    var regs = FakeRegs{};
    const h = handle(ctrl.key_size_128);
    key.rotate(&st, &regs, &h, null);
    try std.testing.expectEqualSlices(u8, "Zwwww", regs.seq());

    regs = .{};
    const iv = key.Iv{ 9, 9, 9, 9 };
    st.enabled = 1;
    key.rotate(&st, &regs, &h, &iv);
    try std.testing.expectEqualSlices(u8, "ZwwwwwwwwA", regs.seq());
    try std.testing.expectEqual(@as(u8, 1), st.enabled);

    regs = .{};
    key.rotate(&st, &regs, &h, null); // cached IV is restaged
    try std.testing.expectEqualSlices(u8, "ZwwwwwwwwA", regs.seq());
    try std.testing.expectEqual(@as(u32, 0x0900_0000), regs.reg03[4]);
}
