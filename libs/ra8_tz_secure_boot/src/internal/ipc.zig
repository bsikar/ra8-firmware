//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! IPC security and privilege attribution, encoded as data.
//!
//! The eight attributable IPC targets are named by the caller; the bit each
//! one occupies in IPCSAR / IPCPAR is this encoder's business alone.

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
pub const shifts: [Target.count]u5 = .{ 0, 1, 8, 9, 16, 17, 18, 19 };

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
        const world = @backingInt(attr.world);
        const access = @backingInt(attr.access);
        if (world > 1 or access > 1) return false;
    }
    return true;
}

/// The map the cpu1_pingpong app runs: everything Secure and Privileged
/// except the two channels the Non-Secure half owns.
pub fn cpu1Pingpong() Attribution {
    var cfg: Attribution = .{};
    cfg.target[@backingInt(Target.channel0)].world = .non_secure;
    cfg.target[@backingInt(Target.channel2)].world = .non_secure;
    return cfg;
}
