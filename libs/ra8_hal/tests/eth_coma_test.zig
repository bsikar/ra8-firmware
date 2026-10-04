//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const coma = @import("eth_coma");

const Fake = struct {
    regs: [4]u32 = [_]u32{0xFFFF_FFFF} ** 4,
    agent: [0x144 / 4]u32 = [_]u32{0} ** (0x144 / 4),

    fn window(f: *Fake, budget: u32) coma.Window {
        return .{ .regs = @intFromPtr(&f.regs), .agent = @intFromPtr(&f.agent), .delay_iters = 0, .bpr_budget = budget };
    }
};

test "reset zeroes CTRL, STS, IE and ICLR" {
    var f: Fake = .{};
    coma.reset(f.window(1));
    try std.testing.expectEqualSlices(u32, &.{ 0, 0, 0, 0 }, &f.regs);
}

test "quiesce zeroes CTRL and IE only" {
    var f: Fake = .{};
    coma.quiesce(f.window(1));
    try std.testing.expectEqualSlices(u32, &.{ 0, 0xFFFF_FFFF, 0, 0xFFFF_FFFF }, &f.regs);
}

test "clearStatus writes ICLR and drops only the masked STS bits" {
    var f: Fake = .{};
    f.regs[1] = 0b1111;
    coma.clearStatus(f.window(1), 0b0101);
    try std.testing.expectEqual(@as(u32, 0b1010), f.regs[1]);
    try std.testing.expectEqual(@as(u32, 0b0101), f.regs[3]);
    try std.testing.expectEqual(@as(u32, 0b1010), coma.status(f.window(1)));
}

test "takeStatus returns STS, mirrors it to ICLR and zeroes STS" {
    var f: Fake = .{};
    f.regs[1] = 0x8001;
    try std.testing.expectEqual(@as(u32, 0x8001), coma.takeStatus(f.window(1)));
    try std.testing.expectEqual(@as(u32, 0x8001), f.regs[3]);
    try std.testing.expectEqual(@as(u32, 0), f.regs[1]);
}

test "resetAndClock leaves RRC released and only the switch clock on" {
    var f: Fake = .{};
    coma.resetAndClock(f.window(1));
    try std.testing.expectEqual(@as(u32, 0), f.agent[coma.off_rrc / 4]);
    try std.testing.expectEqual(coma.rcec_rce, f.agent[coma.off_rcec / 4]);
}

test "kickPool writes BPIOG and waitPool returns once BPR is set" {
    var f: Fake = .{};
    coma.kickPool(f.window(4));
    try std.testing.expectEqual(coma.cabpirm_bpiog, f.agent[coma.off_cabpirm / 4]);
    f.agent[coma.off_cabpirm / 4] |= coma.cabpirm_bpr;
    try coma.waitPool(f.window(4));
}

test "enableAgents turns on RCE and every agent clock" {
    var f: Fake = .{};
    coma.enableAgents(f.window(1));
    try std.testing.expectEqual(coma.rcec_rce | 0x7F, f.agent[coma.off_rcec / 4]);
}

test "bringup times out when BPR never sets and leaves agent clocks off" {
    var f: Fake = .{};
    try std.testing.expectError(error.BprTimeout, coma.bringup(f.window(8)));
    try std.testing.expectEqual(coma.rcec_rce, f.agent[coma.off_rcec / 4]);
}

test "default window carries the hardware bases and C timing" {
    const w: coma.Window = .{};
    try std.testing.expectEqual(@as(usize, 0x403C0000), w.regs);
    try std.testing.expectEqual(@as(usize, 0x403C9000), w.agent);
    try std.testing.expectEqual(@as(u32, 3_000_000), w.delay_iters);
    try std.testing.expectEqual(@as(u32, 1_000_000), w.bpr_budget);
}
