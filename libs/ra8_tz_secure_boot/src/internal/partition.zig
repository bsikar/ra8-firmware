//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Whole-device security attribution, as a descriptor and its validator.
//!
//! Applying a partition is two driver calls; deciding whether a descriptor
//! is even applyable is the part worth stating on its own, so it lives here
//! and is checked before a single register moves.

const std = @import("std");
const regs = @import("regs.zig");

/// Attribute a SAU region carries.
pub const SauAttr = enum(u8) { ns = 0, nsc = 1, _ };

/// One SAU window, mirroring `ra8_sau_region_t`.
pub const SauRegion = extern struct {
    base: usize,
    size: u32,
    attr: SauAttr,
};

/// Bounds a descriptor is checked against.
pub const Limits = struct {
    /// SRAMSABAR0..3.
    pub const sram_bank_count: usize = 4;
    /// SRAMSABARn reserves the low 13 bits, so a boundary is 4 KB aligned.
    pub const sram_granule: u32 = 0x2000;
    /// SAU alignment and minimum size.
    pub const sau_granule: u32 = 32;
    /// One past the top of the 32-bit address space.
    pub const address_ceiling: u64 = 1 << 32;
};

/// Whole-device map. Anything not carved out stays Secure.
pub const Partition = extern struct {
    sau_regions: ?[*]const SauRegion,
    sram_boundary: ?[*]const u32,
    sau_region_count: u8,
    sau_all_ns: bool,
};

/// Check a descriptor against the silicon's limits.
///
/// `implemented` is what `ra8_sau_region_count()` reports, passed in so the
/// decision stays testable without the driver.
pub fn validate(cfg: *const Partition, implemented: u8) u32 {
    if (cfg.sau_region_count != 0 and cfg.sau_regions == null) return regs.Err.null_ptr;
    if (cfg.sau_region_count > implemented) return regs.Err.not_supported;

    if (cfg.sau_regions) |regions| {
        for (regions[0..cfg.sau_region_count]) |region| {
            const err = validateRegion(region);
            if (err != regs.Err.ok) return err;
        }
    }

    if (cfg.sram_boundary) |boundary| {
        for (boundary[0..Limits.sram_bank_count]) |offset| {
            if (offset % Limits.sram_granule != 0) return regs.Err.invalid_arg;
        }
    }
    return regs.Err.ok;
}

/// One window's own rules: granule-aligned, non-empty, a named attribute,
/// and wholly inside the 32-bit address space.
fn validateRegion(region: SauRegion) u32 {
    if (region.base % Limits.sau_granule != 0) return regs.Err.invalid_arg;
    if (region.size == 0 or region.size % Limits.sau_granule != 0) return regs.Err.invalid_arg;
    if (@backingInt(region.attr) > @backingInt(SauAttr.nsc)) return regs.Err.invalid_arg;
    const top: u64 = @as(u64, region.base) + @as(u64, region.size);
    if (top > Limits.address_ceiling) return regs.Err.invalid_arg;
    return regs.Err.ok;
}

/// The EK-RA8D2 board map: NS upper MRAM, NS upper SRAM, NS SDRAM and the
/// NSC veneer alias. MRAM and SRAM are named at their bit-28 Non-secure
/// aliases, because the RA8 IDAU keeps every bit-28-clear address Secure
/// (RA8FW-510). The SDRAM alias is not yet read from the HUM. The SRAM boundary is left as boot ROM set it.
pub const board_regions = [_]SauRegion{
    .{ .base = 0x12080000, .size = 0x80000, .attr = .ns },
    .{ .base = 0x32100000, .size = 0x100000, .attr = .ns },
    .{ .base = 0x6A000000, .size = 0x2000000, .attr = .ns },
    .{ .base = 0x10000000, .size = 0x100000, .attr = .nsc },
};

/// The board map as a descriptor, ready to hand to `apply`.
pub const board_map: Partition = .{
    .sau_regions = &board_regions,
    .sram_boundary = null,
    .sau_region_count = board_regions.len,
    .sau_all_ns = false,
};

test "the board map validates against the silicon it is written for" {
    try std.testing.expectEqual(regs.Err.ok, validate(&board_map, 8));
    try std.testing.expectEqual(@as(u8, 4), board_map.sau_region_count);
    try std.testing.expect(!board_map.sau_all_ns);
    try std.testing.expect(board_map.sram_boundary == null);
}

test "a count the silicon cannot serve is not supported" {
    try std.testing.expectEqual(regs.Err.not_supported, validate(&board_map, 3));
    try std.testing.expectEqual(regs.Err.ok, validate(&board_map, 4));
}

test "a non-zero count with no table is a null pointer" {
    const cfg: Partition = .{
        .sau_regions = null,
        .sram_boundary = null,
        .sau_region_count = 2,
        .sau_all_ns = false,
    };
    try std.testing.expectEqual(regs.Err.null_ptr, validate(&cfg, 8));
}

test "an empty descriptor is legal and asks for nothing" {
    const cfg: Partition = .{
        .sau_regions = null,
        .sram_boundary = null,
        .sau_region_count = 0,
        .sau_all_ns = false,
    };
    try std.testing.expectEqual(regs.Err.ok, validate(&cfg, 0));
}

test "base and size must both sit on the 32-byte granule" {
    try std.testing.expectEqual(regs.Err.invalid_arg, validateRegion(.{
        .base = 0x02080010,
        .size = 0x80000,
        .attr = .ns,
    }));
    try std.testing.expectEqual(regs.Err.invalid_arg, validateRegion(.{
        .base = 0x02080000,
        .size = 0x80010,
        .attr = .ns,
    }));
}

test "a zero-size window is rejected" {
    try std.testing.expectEqual(regs.Err.invalid_arg, validateRegion(.{
        .base = 0x02080000,
        .size = 0,
        .attr = .ns,
    }));
}

test "a window may touch the top of the address space but not cross it" {
    try std.testing.expectEqual(regs.Err.ok, validateRegion(.{
        .base = 0xFFFFF000,
        .size = 0x1000,
        .attr = .ns,
    }));
    try std.testing.expectEqual(regs.Err.invalid_arg, validateRegion(.{
        .base = 0xFFFFF000,
        .size = 0x2000,
        .attr = .ns,
    }));
}

test "an unnamed attribute is a caller error" {
    try std.testing.expectEqual(regs.Err.invalid_arg, validateRegion(.{
        .base = 0x02080000,
        .size = 0x80000,
        .attr = @fromBackingInt(@intCast(9)),
    }));
}

test "an SRAM boundary must be 4 KB aligned in every bank" {
    const good = [_]u32{ 0, 0x2000, 0x4000, 0x6000 };
    const bad = [_]u32{ 0, 0x2000, 0x2001, 0x6000 };
    var cfg = board_map;
    cfg.sram_boundary = &good;
    try std.testing.expectEqual(regs.Err.ok, validate(&cfg, 8));
    cfg.sram_boundary = &bad;
    try std.testing.expectEqual(regs.Err.invalid_arg, validate(&cfg, 8));
}
