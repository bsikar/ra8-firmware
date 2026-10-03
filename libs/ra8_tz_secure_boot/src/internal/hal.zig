//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The HAL seam: the three driver calls the secure boot does not implement.
//!
//! SAU programming and the SRAM security boundary belong to `ra8_hal`, so the
//! membrane declares them rather than reimplementing them. Off target they
//! become a recorded fake, which is what lets the sequencer's ordering be
//! tested on a host that has no registers at all.

const std = @import("std");
const mmio = @import("mmio.zig");
const partition = @import("partition.zig");
const regs = @import("regs.zig");

/// Mirrors `ra8_sau_cfg_t`. Distinct from `partition.Partition`, which also
/// carries the SRAM boundary; the SAU driver takes only its own half.
pub const SauCfg = extern struct {
    regions: ?[*]const partition.SauRegion,
    region_count: u8,
    all_ns: bool,
};

// Both return `ra8_err_t`, which is `enum : uint16_t`; reading the result as
// a u32 would take the undefined upper half of the return register on x86-64.
extern fn ra8_sau_configure(cfg: *const SauCfg) callconv(.c) u16;
extern fn ra8_sau_region_count() callconv(.c) u8;
extern fn ra8_sram_set_boundary(bank: u8, offset: u32) callconv(.c) u16;

/// What the fake driver saw and what it should answer.
pub const Fake = struct {
    /// What `regionCount()` reports.
    implemented: u8 = 8,
    /// What `configure()` returns.
    configure_result: u32 = regs.Err.ok,
    /// What `setBoundary()` returns.
    boundary_result: u32 = regs.Err.ok,

    configure_count: u8 = 0,
    boundary_count: u8 = 0,
    last_region_count: u8 = 0,
    last_all_ns: bool = false,
    last_bank: u8 = 0,
    last_offset: u32 = 0,
};

/// Live fake; untouched on a target build.
pub var fake: Fake = .{};

/// Return the fake to its power-on answers.
pub fn reset() void {
    fake = .{};
}

/// Program the SAU from a descriptor.
pub fn configure(cfg: *const SauCfg) u32 {
    if (comptime mmio.live_registers) return ra8_sau_configure(cfg);
    fake.configure_count +%= 1;
    fake.last_region_count = cfg.region_count;
    fake.last_all_ns = cfg.all_ns;
    return fake.configure_result;
}

/// How many SAU regions this silicon implements.
pub fn regionCount() u8 {
    if (comptime mmio.live_registers) return ra8_sau_region_count();
    return fake.implemented;
}

/// Move one SRAM bank's security boundary.
pub fn setBoundary(bank: u8, offset: u32) u32 {
    if (comptime mmio.live_registers) return ra8_sram_set_boundary(bank, offset);
    fake.boundary_count +%= 1;
    fake.last_bank = bank;
    fake.last_offset = offset;
    return fake.boundary_result;
}

test "the fake answers what it was told to answer" {
    reset();
    fake.implemented = 5;
    try std.testing.expectEqual(@as(u8, 5), regionCount());
    fake.configure_result = regs.Err.not_supported;
    const cfg: SauCfg = .{ .regions = null, .region_count = 0, .all_ns = false };
    try std.testing.expectEqual(regs.Err.not_supported, configure(&cfg));
    try std.testing.expectEqual(@as(u8, 1), fake.configure_count);
}

test "reset clears a dirtied fake" {
    reset();
    _ = setBoundary(2, 0x4000);
    try std.testing.expectEqual(@as(u8, 2), fake.last_bank);
    reset();
    try std.testing.expectEqual(@as(u8, 0), fake.boundary_count);
    try std.testing.expectEqual(@as(u8, 8), fake.implemented);
}
