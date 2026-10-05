//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Anti-rollback counter (ARC) logic, moved out of ra8_flash_config.c
//! (RA8FW-802). HUM Ch 59.7.4. SEC and NSEC counts are popcounts of the
//! OFS words; OEMBL goes through the MACI read so RSIP can intercept it.

pub const arc_sec: u8 = 0;
pub const arc_oembl: u8 = 1;
pub const arc_nsec_0: u8 = 2;
pub const arc_count: u8 = 6;

pub const mram_base: usize = 0x4013C000;
pub const off_mastat: usize = 0x2010;
pub const off_mcntselr: usize = 0x2048;
pub const off_mcntdtr0: usize = 0x204C;
pub const off_mcntdtr1: usize = 0x2050;
pub const mastat_cmdlk: u8 = 0x10;
pub const mcntselr_mask: u8 = 0x07;

pub const cmd_increment: u8 = 0x35;
pub const cmd_read: u8 = 0x39;
pub const cmd_final: u8 = 0xD0;
pub const spin_limit: u32 = 0x00100000;

pub const arccs_addr: usize = 0x02E17932;
pub const sec_addr: usize = 0x02F27E00;
pub const nsec_addr: usize = 0x02F27E08;
pub const sec_max: u32 = 64;
pub const oembl_max: u32 = 64;
pub const nsec_single: u32 = 256;
pub const nsec_multiple: u32 = 64;
pub const max_words: u32 = 16;

/// MCNTSELR value for an ARC id (0 for anything out of range).
pub fn mcntselr(id: u8) u8 {
    return switch (id) {
        arc_sec => 1,
        arc_oembl => 2,
        2...5 => 4 + (id - arc_nsec_0),
        else => 0,
    };
}

/// ARCCS.ARCNS == 1 selects one 256-bit NSEC counter.
pub fn nsecSingle(arccs: u16) bool {
    return arccs & 0x03 == 1;
}

pub fn maxCount(id: u8, arccs: u16) u32 {
    if (id == arc_sec) return sec_max;
    if (id == arc_oembl) return oembl_max;
    return if (nsecSingle(arccs)) nsec_single else nsec_multiple;
}

/// Words per NSEC counter and the first word index for `id`.
pub fn nsecSpan(id: u8, arccs: u16) struct { first: u32, words: u32 } {
    const per: u32 = @min(if (nsecSingle(arccs)) @as(u32, 16) else 2, max_words);
    const slot: u32 = switch (id) {
        2 => 0,
        3 => 1,
        4 => 2,
        else => 3,
    };
    return .{ .first = per * slot, .words = per };
}

/// Sum of set bits over `n` consecutive u32 words at `addr`.
pub fn popWords(hw: anytype, addr: usize, n: u32) u32 {
    var count: u32 = 0;
    var w: u32 = 0;
    while (w < n) : (w += 1) count += @popCount(hw.read32(addr + @as(usize, w) * 4));
    return count;
}

/// Count for SEC or an NSEC counter, read straight from OFS.
pub fn ofsCount(hw: anytype, id: u8) u32 {
    if (id == arc_sec) return popWords(hw, sec_addr, sec_max >> 5);
    const span = nsecSpan(id, hw.read16(arccs_addr));
    return popWords(hw, nsec_addr + @as(usize, span.first) * 4, span.words);
}
