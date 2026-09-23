//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure core of the RA8D2 TrustZone secure boot: the register addresses and
//! field encodings, the canonical five-region SAU partition, the guard
//! predicates, the NS root-of-trust header layout, and the host-side capture
//! model the unit tests inspect. Nothing here performs MMIO or exports a
//! symbol; `../ra8_tz_secure_boot_abi.zig` owns both.
//!
//! Addresses follow the RA8D2 HUM (Ch 3.2.1 IPCSAR, Ch 3.2.2 IPCPAR,
//! Ch 13.2.1 PRCR_S) and the Armv8-M ARM (D.4.5 SAU_CTRL, D.4.7 SAU_RLAR,
//! B3.2.4 VTOR_NS).

const std = @import("std");

/// Boot progress marker (`ra8_tz_secure_boot_step_t`).
pub const Step = enum(u8) {
    idle = 0,
    sau_done = 1,
    prcr_unlocked = 2,
    ipcsar_written = 3,
    prcr_relocked = 4,
    blxns_armed = 5,
    branched = 6,
};

/// Number of SAU regions the canonical partition programmes
/// (`k_ra8_tz_sau_region_count`).
pub const region_count: u8 = 5;

/// Memory-mapped registers the secure boot drives
/// (`ra8_tz_secure_boot_addr_t`).
pub const Addr = struct {
    /// SAU Control.
    pub const sau_ctrl: usize = 0xE000EDD0;
    /// SAU Type.
    pub const sau_type: usize = 0xE000EDD4;
    /// SAU Region Number.
    pub const sau_rnr: usize = 0xE000EDD8;
    /// SAU Region Base.
    pub const sau_rbar: usize = 0xE000EDDC;
    /// SAU Region Limit.
    pub const sau_rlar: usize = 0xE000EDE0;
    /// Non-Secure alias of SCB->VTOR.
    pub const scb_vtor_ns: usize = 0xE002ED08;
    /// CPSCU IPCSAR.
    pub const ipcsar: usize = 0x40008610;
    /// CPSCU IPCPAR.
    pub const ipcpar: usize = 0x40008614;
    /// SYSC PRCR_S (16-bit).
    pub const prcr_s: usize = 0x4001E3FA;
};

/// SAU_CTRL.ENABLE.
pub const sau_ctrl_enable: u32 = 0x00000001;
/// SAU_CTRL.ALLNS, deliberately left clear (default-deny).
pub const sau_ctrl_allns: u32 = 0x00000002;
/// SAU_RLAR.ENABLE.
pub const sau_rlar_enable: u32 = 0x00000001;
/// SAU_RLAR.NSC.
pub const sau_rlar_nsc: u32 = 0x00000002;
/// SAU_TYPE.SREGION occupies the low byte.
pub const sau_type_mask: u32 = 0x000000FF;

/// PRCR_S write key in the top byte.
pub const prcr_s_key: u16 = 0xA500;
/// PRCR_S.PRC4, the CPSCU attribution gate.
pub const prcr_s_prc4_open: u16 = 0x0010;
/// Key | PRC4: opens the gate.
pub const prcr_s_open: u16 = 0xA510;
/// Key with PRC4 clear: closes the gate.
pub const prcr_s_close: u16 = 0xA500;

/// Byte offset of the NS RoT header from the NS image base
/// (`k_ra8_tz_ns_rot_header_offset`), just past the 16-slot NS vector table.
pub const ns_rot_header_offset: usize = 0x40;
/// ASCII "NSR1" little-endian (`k_ra8_tz_ns_rot_header_magic`).
pub const ns_rot_header_magic: u32 = 0x3152534E;

/// Self-describing header the NS linker embeds (`ra8_ns_rot_header_t`).
pub const NsRotHeader = extern struct {
    magic: u32,
    body_len: u32,
};

comptime {
    std.debug.assert(@sizeOf(NsRotHeader) == 8);
    std.debug.assert(@offsetOf(NsRotHeader, "magic") == 0);
    std.debug.assert(@offsetOf(NsRotHeader, "body_len") == 4);
    // The header must land past the 16-slot Armv8-M NS vector table and stay
    // word-aligned: the whole cross-image contract rests on both.
    std.debug.assert(ns_rot_header_offset == 16 * @sizeOf(u32));
    std.debug.assert(ns_rot_header_offset % @alignOf(NsRotHeader) == 0);
}

/// One programmed SAU region of the canonical partition.
pub const RegionSpec = struct {
    index: u8,
    base: u32,
    limit: u32,
    is_nsc: bool,
};

/// The canonical RA8D2 partition (`ra8_tz_secure_boot_partition_t`), in the
/// order `sau_init` programmes it. Region 0 and 2 point at the unused IDAU
/// aliases rather than the real `.gnu.sgstubs` placement, which bench work
/// found bricks the chip.
pub const partition = [_]RegionSpec{
    .{ .index = 0, .base = 0x10000000, .limit = 0x100FFFE0, .is_nsc = true },
    .{ .index = 1, .base = 0x02080000, .limit = 0x020FFFE0, .is_nsc = false },
    .{ .index = 2, .base = 0x12000000, .limit = 0x1200FFE0, .is_nsc = true },
    .{ .index = 3, .base = 0x22100000, .limit = 0x221FFFE0, .is_nsc = false },
    .{ .index = 4, .base = 0x50000000, .limit = 0x5FFFFFE0, .is_nsc = false },
};

comptime {
    std.debug.assert(partition.len == region_count);
}

/// RLAR word for one region: limit with ENABLE, plus NSC when asked.
pub fn rlarFor(limit: u32, is_nsc: bool) u32 {
    var rlar = limit | sau_rlar_enable;
    if (is_nsc) rlar |= sau_rlar_nsc;
    return rlar;
}

/// True when SAU_TYPE reports enough implemented regions for the partition.
pub fn sauRegionsSufficient(sau_type: u32) bool {
    return (sau_type & sau_type_mask) >= @as(u32, region_count);
}

/// True when a PRCR_S write opens the PRC4 gate rather than closing it.
pub fn prcrOpensGate(value: u16) bool {
    return (value & prcr_s_prc4_open) != 0;
}

/// The NS reset vector values the boot refuses: all-zero and erased MRAM.
pub fn resetEntryBogus(reset_entry: u32) bool {
    return reset_entry == 0 or reset_entry == std.math.maxInt(u32);
}

/// Armv8-M vector tables are word-aligned.
pub fn pointerAligned4(addr: usize) bool {
    return (addr & 0x3) == 0;
}

/// BLXNS switches world only when bit 0 of the target is clear; the NS reset
/// vector carries the Thumb bit, so it is stripped before the branch.
pub fn nsEntryFromResetVector(reset_entry: u32) u32 {
    return reset_entry & ~@as(u32, 1);
}

/// True when the word at the fixed offset is the NS RoT header marker.
pub fn headerMagicOk(magic: u32) bool {
    return magic == ns_rot_header_magic;
}

/// True when no two programmed regions overlap, walked pairwise.
pub fn partitionDisjoint() bool {
    for (partition, 0..) |a, i| {
        for (partition[i + 1 ..]) |b| {
            if (a.base <= b.limit and b.base <= a.limit) return false;
        }
    }
    return true;
}

/// True when every base and limit sits on the Armv8-M 32-byte SAU quantum.
pub fn partitionQuantumAligned() bool {
    for (partition) |spec| {
        if (spec.base & 0x1F != 0) return false;
        if (spec.limit & 0x1F != 0) return false;
    }
    return true;
}

/// True when both NSC aliases stay clear of the Secure lower-MRAM block,
/// which is the invariant `project_sau_sgstubs_brick` exists to protect.
pub fn nscAliasesOutsideSecureMram() bool {
    const secure_mram_end: u32 = 0x0207FFFF;
    for (partition) |spec| {
        if (!spec.is_nsc) continue;
        if (spec.base <= secure_mram_end) return false;
        if (spec.limit <= secure_mram_end) return false;
    }
    return true;
}

/// Host-side capture of every boot side-effect
/// (`ra8_tz_secure_boot_host_state_t`). On a hosted build the register
/// helpers write here instead of touching memory, so the unit tests can
/// assert the documented sequence.
pub const HostState = struct {
    sau_ctrl: u32 = 0,
    sau_region_base: [region_count]u32 = .{0} ** region_count,
    sau_region_limit: [region_count]u32 = .{0} ** region_count,
    sau_region_nsc: [region_count]u8 = .{0} ** region_count,
    prcr_s_last: u16 = 0,
    prcr_unlock_count: u8 = 0,
    prcr_relock_count: u8 = 0,
    ipcsar_value: u32 = 0,
    ipcpar_value: u32 = 0,
    blxns_target: u32 = 0,
    blxns_msp_ns: u32 = 0,
    vtor_ns: u32 = 0,

    /// Clear every capture back to the documented baseline.
    pub fn reset(self: *HostState) void {
        self.* = .{};
    }

    /// Capture a 32-bit register write. SAU RBAR / RLAR are routed through
    /// `noteRegion` instead, exactly as the C did.
    pub fn write32(self: *HostState, addr: usize, value: u32) void {
        switch (addr) {
            Addr.ipcsar => self.ipcsar_value = value,
            Addr.ipcpar => self.ipcpar_value = value,
            Addr.scb_vtor_ns => self.vtor_ns = value,
            Addr.sau_ctrl => self.sau_ctrl = value,
            else => {},
        }
    }

    /// Canned register reads: SAU_TYPE reports the Cortex-M85's 8 regions,
    /// IPCSAR reads back what was written, everything else reads 0.
    pub fn read32(self: *const HostState, addr: usize) u32 {
        if (addr == Addr.sau_type) return 8;
        if (addr == Addr.ipcsar) return self.ipcsar_value;
        return 0;
    }

    /// Capture a PRCR_S write and count it as an unlock or a relock. The
    /// counters wrap in a byte, as the C's `(uint8_t)(n + 1)` did.
    pub fn write16(self: *HostState, addr: usize, value: u16) void {
        _ = addr;
        self.prcr_s_last = value;
        if (prcrOpensGate(value)) {
            self.prcr_unlock_count +%= 1;
        } else {
            self.prcr_relock_count +%= 1;
        }
    }

    /// Capture one programmed SAU region; out-of-range indices are ignored.
    pub fn noteRegion(self: *HostState, region: u8, base: u32, limit: u32, is_nsc: bool) void {
        if (region >= region_count) return;
        self.sau_region_base[region] = base;
        self.sau_region_limit[region] = limit;
        self.sau_region_nsc[region] = if (is_nsc) 1 else 0;
    }
};
