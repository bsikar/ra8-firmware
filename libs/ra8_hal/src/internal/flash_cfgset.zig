//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Start-up area and configuration-set words moved out of
//! ra8_flash_config.c (RA8FW-808). HUM Ch 7 p 278, Ch 59.7.4.5 / 59.7.4.8.

pub const mram_base: usize = 0x4013C000;
pub const off_msaddr: usize = 0x2030;
pub const off_mstatr: usize = 0x2080;
pub const off_msuasmon: usize = 0x20DC;
pub const off_msuacr: usize = 0x20E8;

pub const ofs_start: u32 = 0x02C9F000;
pub const ofs_size: u32 = 0x00001000;
pub const extra_start: u32 = 0x02E07600;
pub const extra_size: u32 = 0x00010400;
pub const startup_addr: u32 = 0x02C9F070;

pub const word_count: usize = 8;
pub const cmd_program: u8 = 0xE8;
pub const cmd_config_set: u8 = 0x40;
pub const cmd_word_count: u8 = 0x08;
pub const cmd_final: u8 = 0xD0;
pub const maci_spin_limit: u32 = 0x00100000;
pub const mstatr_any_err: u32 = 0x00B85020;

/// ra8_flash_startup_t: default 0, alternate 1, btflg 2 (the last valid one).
pub const startup_default: u8 = 0;
pub const startup_alternate: u8 = 1;
pub const startup_max: u8 = 2;

pub fn reg(off: usize) usize {
    return mram_base + off;
}

pub const Region = enum { none, ofs, extra };

/// Which MACI target a configuration-set address falls in.
pub fn region(addr: u32) Region {
    if (addr >= ofs_start and addr - ofs_start < ofs_size) return .ofs;
    if (addr >= extra_start and addr - extra_start < extra_size) return .extra;
    return .none;
}

/// Program (0xE8) for the extra-MRAM data area, Configuration Set (0x40)
/// for OFS; Config-Set leaves the data area blank and sets CFGSETERR.
pub fn opener(r: Region) u8 {
    return if (r == .extra) cmd_program else cmd_config_set;
}

/// MSUACR: key 0x6600 with SAS bit 0 set for the alternate area.
pub fn msuacrWord(target: u8) u16 {
    return 0x6600 | @as(u16, @intFromBool(target == startup_alternate));
}

/// The BTFLG configuration set: all ones, word 3 bit 15 = 1 for default.
pub fn startupWords(target: u8) [word_count]u16 {
    var words = [_]u16{0xFFFF} ** word_count;
    const btflg: u16 = if (target == startup_default) 0x8000 else 0x0000;
    words[3] = btflg | 0x1FFF;
    return words;
}

pub const Flags = struct { btflg: u8, fspr: u8 };

/// MSUASMON: BTFLG in bit 31, FSPR in bit 15.
pub fn startupFlags(v: u32) Flags {
    return .{
        .btflg = @intFromBool(v & 0x80000000 != 0),
        .fspr = @intFromBool(v & 0x00008000 != 0),
    };
}
