//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The canonical five-region secure-boot SAU partition, stated as data.
//!
//! Distinct from `partition.board_regions`, which is the four-window map a
//! caller may hand to `ra8_tz_partition_apply()`. This is the fixed partition
//! the reset path programs before it ever reaches Non-Secure code, and its
//! region ORDER is a published contract: a bench SWD dump of SAU_RNR indexes
//! against it, so the windows are named by index rather than merely listed.

const std = @import("std");
const partition = @import("partition.zig");
const regs = @import("regs.zig");

/// Index each window occupies in the programmed partition.
///
/// Published contract. Do not reorder.
pub const Region = enum(u8) {
    /// NSC alias for code-flash veneers.
    code_nsc = 0,
    /// Non-Secure upper MRAM, where the NS image lives.
    ns_mram = 1,
    /// NSC alias for SRAM veneers.
    sram_nsc = 2,
    /// Non-Secure upper SRAM, the NS image's data.
    ns_sram = 3,
    /// Non-Secure peripheral window.
    ns_periph = 4,

    /// Windows the reset path programs.
    pub const count: usize = 5;
};

/// The five windows, in `Region` order.
///
/// Stated as base and size, the way the linker script states them, rather
/// than as the pre-decremented RLAR limits this partition used to carry:
/// `ra8_sau_configure()` derives `base + size - 32` once, so the 32-byte
/// quantum is the driver's arithmetic and not a constant kept correct by
/// hand.
pub const regions = [Region.count]partition.SauRegion{
    .{ .base = 0x10000000, .size = 0x00100000, .attr = .nsc },
    .{ .base = 0x12080000, .size = 0x00080000, .attr = .ns },
    .{ .base = 0x12000000, .size = 0x00010000, .attr = .nsc },
    .{ .base = 0x32100000, .size = 0x00100000, .attr = .ns },
    .{ .base = 0x50000000, .size = 0x10000000, .attr = .ns },
};

/// Unmapped memory stays Secure. The whole boot depends on this default-deny
/// posture, so it is named rather than left to `SAU_CTRL`'s reset value.
pub const all_ns: bool = false;

/// Whether silicon reporting `implemented` regions can hold this partition.
///
/// The shortfall is checked before the driver is called so it keeps reporting
/// `not_supported`, which is `ra8_tz_secure_boot_sau_init()`'s published
/// contract rather than the driver's `invalid_arg`.
pub fn supported(implemented: u8) bool {
    return implemented >= Region.count;
}

/// This partition as a descriptor, so the validator that guards
/// `ra8_tz_partition_apply()` can be pointed at the reset path's own map too.
pub const descriptor: partition.Partition = .{
    .sau_regions = &regions,
    .sram_boundary = null,
    .sau_region_count = Region.count,
    .sau_all_ns = all_ns,
};

/// The window at a published index.
pub fn window(region: Region) partition.SauRegion {
    return regions[@intFromEnum(region)];
}

test "the published region order is what the table actually holds" {
    try std.testing.expectEqual(@as(usize, 5), Region.count);
    try std.testing.expectEqual(@as(usize, 0x10000000), window(.code_nsc).base);
    try std.testing.expectEqual(@as(usize, 0x12080000), window(.ns_mram).base);
    try std.testing.expectEqual(@as(usize, 0x12000000), window(.sram_nsc).base);
    try std.testing.expectEqual(@as(usize, 0x32100000), window(.ns_sram).base);
    try std.testing.expectEqual(@as(usize, 0x50000000), window(.ns_periph).base);
}

test "only the two veneer aliases are NSC" {
    try std.testing.expectEqual(partition.SauAttr.nsc, window(.code_nsc).attr);
    try std.testing.expectEqual(partition.SauAttr.nsc, window(.sram_nsc).attr);
    try std.testing.expectEqual(partition.SauAttr.ns, window(.ns_mram).attr);
    try std.testing.expectEqual(partition.SauAttr.ns, window(.ns_sram).attr);
    try std.testing.expectEqual(partition.SauAttr.ns, window(.ns_periph).attr);
}

test "the reset path's own partition passes the validator that guards apply" {
    try std.testing.expectEqual(regs.Err.ok, partition.validate(&descriptor, Region.count));
}

test "silicon short of five regions cannot hold this partition" {
    try std.testing.expect(!supported(4));
    try std.testing.expect(supported(5));
    try std.testing.expect(supported(8));
    try std.testing.expectEqual(
        regs.Err.not_supported,
        partition.validate(&descriptor, 4),
    );
}

test "no two windows overlap" {
    for (regions, 0..) |mine, i| {
        const my_top = mine.base + mine.size;
        for (regions[i + 1 ..]) |theirs| {
            const their_top = theirs.base + theirs.size;
            try std.testing.expect(my_top <= theirs.base or their_top <= mine.base);
        }
    }
}

test "unmapped memory stays Secure" {
    try std.testing.expect(!all_ns);
    try std.testing.expect(!descriptor.sau_all_ns);
}

test "the NS image window is the upper half of MRAM, not the whole bank" {
    const ns_mram = window(.ns_mram);
    try std.testing.expectEqual(@as(u32, 0x80000), ns_mram.size);
    try std.testing.expect(ns_mram.base > 0x12000000);
}

test "the NS windows sit at the bit-28 Non-secure aliases" {
    // On RA8 the IDAU makes an address with bit 28 clear Secure, and the SAU
    // cannot make it Non-secure (RA8FW-510), so the NS windows name the alias.
    const alias_bit: u32 = 1 << 28;
    try std.testing.expect(window(.ns_mram).base & alias_bit != 0);
    try std.testing.expect(window(.ns_sram).base & alias_bit != 0);
    try std.testing.expect(window(.ns_periph).base & alias_bit != 0);
}
