//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Reset-cause decode, flag clearing and reset-source masks (RA8FW-752),
//! ported from ra8_reset.c. Register access goes through `hw`, which takes
//! byte offsets from the SYSC base, so tests can use a plain register file.
//! HUM Ch 6.2 "Register Descriptions" (RSTSR0..3, SYRSTMSK0..2, RSTSAR).

pub const codes = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const null_ptr: u16 = 0x504;
};

pub const sysc_base: usize = 0x4001_E000;
pub const aircr_addr: usize = 0xE000_ED0C;
/// AIRCR VECTKEY (0x05FA << 16) | SYSRESETREQ: the software-reset request.
pub const aircr_reset: u32 = (0x05FA << 16) | 0x4;

pub const off = struct {
    pub const rstsr1: usize = 0x0C0;
    pub const rstsar: usize = 0x3C4;
    pub const prcr: usize = 0x3FA;
    pub const rstsr0: usize = 0xA40;
    pub const rstsr2: usize = 0xA44;
    pub const rstsr3: usize = 0xA48;
    pub const syrstmsk0: usize = 0xAD0;
    pub const syrstmsk1: usize = 0xAD4;
    pub const syrstmsk2: usize = 0xAD8;
};

/// Mirror of ra8_reset_raw_t.
pub const Raw = extern struct {
    rstsr0: u8 = 0,
    rstsr1: u32 = 0,
    rstsr2: u8 = 0,
    rstsr3: u8 = 0,
};

comptime {
    if (@sizeOf(Raw) != 12 or @offsetOf(Raw, "rstsr1") != 4 or @offsetOf(Raw, "rstsr2") != 8 or @offsetOf(Raw, "rstsr3") != 9)
        @compileError("Raw no longer matches ra8_reset_raw_t");
}

/// ra8_reset_cause_t values.
pub const cause = struct {
    pub const unknown: u8 = 0;
    pub const warm_start: u8 = 22;
};

const Flag = struct { mask: u32, cause: u8 };

/// RSTSR0 flags in priority order: PORF, LVD0/1/2/4/5RF, DPSRSTF.
const rstsr0_flags = [_]Flag{
    .{ .mask = 0x01, .cause = 1 }, .{ .mask = 0x02, .cause = 2 }, .{ .mask = 0x04, .cause = 3 },
    .{ .mask = 0x08, .cause = 4 }, .{ .mask = 0x20, .cause = 5 }, .{ .mask = 0x40, .cause = 6 },
    .{ .mask = 0x80, .cause = 7 },
};
/// RSTSR3 flags in priority order: CVMRF, OCPRF, TEMPRF.
const rstsr3_flags = [_]Flag{ .{ .mask = 0x01, .cause = 8 }, .{ .mask = 0x10, .cause = 9 }, .{ .mask = 0x80, .cause = 10 } };
/// RSTSR1 flags in priority order: IWDT, WDT0, SW, CLU0, LM0, BUSS, CM,
/// WDT1, CLU1, LM1, NW.
const rstsr1_flags = [_]Flag{
    .{ .mask = 0x1, .cause = 11 },      .{ .mask = 0x2, .cause = 12 },      .{ .mask = 0x4, .cause = 13 },
    .{ .mask = 0x10, .cause = 14 },     .{ .mask = 0x20, .cause = 15 },     .{ .mask = 0x400, .cause = 16 },
    .{ .mask = 0x4000, .cause = 17 },   .{ .mask = 0x20000, .cause = 18 },  .{ .mask = 0x100000, .cause = 19 },
    .{ .mask = 0x200000, .cause = 20 }, .{ .mask = 0x400000, .cause = 21 },
};

fn first(flags: []const Flag, value: u32) u8 {
    for (flags) |f| if (value & f.mask != 0) return f.cause;
    return cause.unknown;
}

pub fn readRaw(hw: anytype) Raw {
    return .{
        .rstsr0 = hw.read8(off.rstsr0),
        .rstsr1 = hw.read32(off.rstsr1),
        .rstsr2 = hw.read8(off.rstsr2),
        .rstsr3 = hw.read8(off.rstsr3),
    };
}

/// Primary cause: RSTSR0, then RSTSR3, then RSTSR1, then RSTSR2.CWSF.
pub fn decode(raw: Raw) u8 {
    const c0 = first(&rstsr0_flags, raw.rstsr0);
    if (c0 != cause.unknown) return c0;
    const c3 = first(&rstsr3_flags, raw.rstsr3);
    if (c3 != cause.unknown) return c3;
    const c1 = first(&rstsr1_flags, raw.rstsr1);
    if (c1 != cause.unknown) return c1;
    return if (raw.rstsr2 & 0x01 != 0) cause.warm_start else cause.unknown;
}

/// mask bits 0..7 select RSTSR0 flags, bits 8..30 select RSTSR1 flags
/// (shifted down by 8), bit 31 sets RSTSR2.CWSF. RSTSR0/1 flags clear by
/// writing 0 over a flag read as 1; CWSF is set by writing 1.
pub fn clear(hw: anytype, mask: u32) void {
    const c0: u8 = @truncate(mask & 0xFF);
    if (c0 != 0) hw.write8(off.rstsr0, hw.read8(off.rstsr0) & ~c0);
    const c1 = (mask & 0x7FFF_FF00) >> 8;
    if (c1 != 0) hw.write32(off.rstsr1, hw.read32(off.rstsr1) & ~c1);
    if (mask & 0x8000_0000 != 0) hw.write8(off.rstsr2, 0x01);
}

pub const Loc = struct { reg: usize, mask: u8 };

/// ra8_reset_source_t -> SYRSTMSKn bit; null for count and beyond.
pub fn sourceLoc(source: u8) ?Loc {
    const table = [_]Loc{
        .{ .reg = off.syrstmsk0, .mask = 0x01 }, .{ .reg = off.syrstmsk0, .mask = 0x02 },
        .{ .reg = off.syrstmsk0, .mask = 0x04 }, .{ .reg = off.syrstmsk0, .mask = 0x10 },
        .{ .reg = off.syrstmsk0, .mask = 0x20 }, .{ .reg = off.syrstmsk0, .mask = 0x40 },
        .{ .reg = off.syrstmsk0, .mask = 0x80 }, .{ .reg = off.syrstmsk1, .mask = 0x02 },
        .{ .reg = off.syrstmsk1, .mask = 0x10 }, .{ .reg = off.syrstmsk1, .mask = 0x20 },
        .{ .reg = off.syrstmsk2, .mask = 0x01 }, .{ .reg = off.syrstmsk2, .mask = 0x02 },
    };
    return if (source < table.len) table[source] else null;
}

const prcr_key: u16 = 0xA500;
const prcr_prc5: u16 = 0x0020;

/// Unlock PRCR.PRC5 (HUM Ch 11.2.18), set (disable) or clear (enable) the
/// mask bit, then relock PRC5 keeping the other PR bits.
pub fn setSourceMask(hw: anytype, loc: Loc, disable: bool) void {
    hw.write16(off.prcr, prcr_key | (hw.read16(off.prcr) & 0xFF) | prcr_prc5);
    const before = hw.read8(loc.reg);
    hw.write8(loc.reg, if (disable) before | loc.mask else before & ~loc.mask);
    hw.write16(off.prcr, prcr_key | (hw.read16(off.prcr) & 0xFF & ~prcr_prc5));
}

pub fn sourceMasked(hw: anytype, loc: Loc) bool {
    return hw.read8(loc.reg) & loc.mask != 0;
}
