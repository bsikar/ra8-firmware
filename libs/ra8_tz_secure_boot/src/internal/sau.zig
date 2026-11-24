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
    /// Non-Secure peripheral window: the bit-28 NS alias
    /// 0x50000000..0x5FFFFFFF, and only that.
    ///
    /// It does not reach down into 0x40000000..0x4FFFFFFF. The IDAU keeps
    /// every bit-28-clear address Secure on the SECEXT core (RA8FW-510), so
    /// the canonical partition cannot mark IPC's secure base 0x40020000 as
    /// NS, and the Secure side reaches IPC there directly. An NS-only core
    /// (CPU1) talks to IPC through its own SAU init instead: the
    /// cpu1_pingpong_ipc example's cpu1_main.c programmes 0x40000000 size
    /// 0x10000000 as NS on CPU1's own SAU (bench evidence 2026-05-27: with a
    /// 0x5xxxxxxx-only CPU1 view, CPU1 SecureFaults on IPC receive even with
    /// IPCSAR=0x50000; extending CPU1's own region down to 0x40000000
    /// unblocks the round-trip). RA8FW-357.
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
    return regions[@backingInt(region)];
}
