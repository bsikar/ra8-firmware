//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! IPC security and privilege attribution, encoded as data.
//!
//! The eight attributable IPC targets are named by the caller; the bit each
//! one occupies in IPCSAR / IPCPAR is this encoder's business alone.

const std = @import("std");
const regs = @import("regs.zig");

/// Which world may reach a target. Secure is 0, matching cold reset.
pub const World = enum(u8) { secure = 0, non_secure = 1, _ };

/// Which privilege level may reach a target. Privileged is 0.
pub const Access = enum(u8) { privileged = 0, unprivileged = 1, _ };

/// The eight independently attributable targets, in register order.
pub const Target = enum(u8) {
    sem_low = 0,
    sem_high = 1,
    nmi_unit0 = 2,
    nmi_unit1 = 3,
    channel0 = 4,
    channel1 = 5,
    channel2 = 6,
    channel3 = 7,

    /// Entries in a whole-device map.
    pub const count: usize = 8;
};

/// The two independent answers for one target.
pub const TargetAttr = extern struct {
    world: World = .secure,
    access: Access = .privileged,
};

/// Whole-device map. Every target is named, so the encoder describes both
/// registers completely and there is no "the rest keep what was there" case.
pub const Attribution = extern struct {
    target: [Target.count]TargetAttr = @splat(.{}),
};

/// Bit position each target encodes to. Shared by IPCSAR and IPCPAR: the two
/// registers ask different questions over the same bit layout.
const shifts: [Target.count]u5 = .{ 0, 1, 8, 9, 16, 17, 18, 19 };

/// The IPCSAR / IPCPAR word pair a map encodes to.
pub const Words = struct { ipcsar: u32 = 0, ipcpar: u32 = 0 };

/// Encode a map into its register words.
pub fn encode(cfg: Attribution) Words {
    var out: Words = .{};
    for (cfg.target, shifts) |attr, shift| {
        if (attr.world == .non_secure) out.ipcsar |= @as(u32, 1) << shift;
        if (attr.access == .unprivileged) out.ipcpar |= @as(u32, 1) << shift;
    }
    return out;
}

/// True when every field of `cfg` holds a value its enum names. The C ABI
/// hands these in as plain bytes, so an out-of-range one is a caller error.
pub fn valid(cfg: Attribution) bool {
    for (cfg.target) |attr| {
        const world = @intFromEnum(attr.world);
        const access = @intFromEnum(attr.access);
        if (world > 1 or access > 1) return false;
    }
    return true;
}

/// The map the cpu1_pingpong app runs: everything Secure and Privileged
/// except the two channels the Non-Secure half owns.
pub fn cpu1Pingpong() Attribution {
    var cfg: Attribution = .{};
    cfg.target[@intFromEnum(Target.channel0)].world = .non_secure;
    cfg.target[@intFromEnum(Target.channel2)].world = .non_secure;
    return cfg;
}

test "a zeroed map encodes to the cold-reset word pair" {
    const words = encode(.{});
    try std.testing.expectEqual(@as(u32, 0), words.ipcsar);
    try std.testing.expectEqual(@as(u32, 0), words.ipcpar);
}

test "each target lands on its own bit, and the two registers share the layout" {
    for (shifts, 0..) |shift, index| {
        var cfg: Attribution = .{};
        cfg.target[index].world = .non_secure;
        cfg.target[index].access = .unprivileged;
        const words = encode(cfg);
        try std.testing.expectEqual(@as(u32, 1) << shift, words.ipcsar);
        try std.testing.expectEqual(@as(u32, 1) << shift, words.ipcpar);
    }
}

test "world and access are answered independently" {
    var cfg: Attribution = .{};
    cfg.target[@intFromEnum(Target.sem_high)].world = .non_secure;
    const words = encode(cfg);
    try std.testing.expectEqual(@as(u32, 1) << 1, words.ipcsar);
    try std.testing.expectEqual(@as(u32, 0), words.ipcpar);
}

test "cpu1_pingpong hands out channel0 and channel2 only" {
    const words = encode(cpu1Pingpong());
    try std.testing.expectEqual((@as(u32, 1) << 16) | (@as(u32, 1) << 18), words.ipcsar);
    try std.testing.expectEqual(@as(u32, 0), words.ipcpar);
}

test "an out-of-range byte is rejected before it can be encoded" {
    var cfg: Attribution = .{};
    cfg.target[0].world = @enumFromInt(7);
    try std.testing.expect(!valid(cfg));
    try std.testing.expect(valid(.{}));
}

test "every shift is distinct" {
    var seen: u32 = 0;
    for (shifts) |shift| {
        const bit = @as(u32, 1) << shift;
        try std.testing.expectEqual(@as(u32, 0), seen & bit);
        seen |= bit;
    }
    _ = regs.Err.ok;
}
