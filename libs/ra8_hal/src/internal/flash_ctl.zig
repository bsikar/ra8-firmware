//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MRAM control words moved out of ra8_flash_config.c (RA8FW-806):
//! W-HUK zeroize, MSAR, ECC encoder/decoder, error address reads and the
//! update transfer, the MSUINITR kick and the clock-frequency notices
//! (RA8FW-807). HUM Ch 59.

pub const mram_base: usize = 0x4013C000;
pub const off_mrcfreq: usize = 0x0004;
pub const off_mrefreq: usize = 0x0008;
pub const off_mrcdecc: usize = 0x0010;
pub const off_mrcrtea: usize = 0x001C;
pub const off_mrcrdea: usize = 0x0020;
pub const off_mrertea: usize = 0x003C;
pub const off_mrerdea: usize = 0x0040;
pub const off_msar: usize = 0x0100;
pub const off_mrezs: usize = 0x0400;
pub const off_mrezc: usize = 0x0404;
pub const off_mctrcntr: usize = 0x2060;
pub const off_mctrlsr: usize = 0x2064;
pub const off_mctrstatr: usize = 0x206C;
pub const off_msuinitr: usize = 0x208C;
pub const off_mrcpea: usize = 0x3018;
pub const off_mrceecc: usize = 0x3804;

pub const mrezc_full_zero: u16 = 0xA505;
pub const mrezs_whukexe: u8 = 0x02;
pub const zeroize_spin: u32 = 0x00400000;
pub const max_list_select: u8 = 0x0F;
pub const mctrlsr_list_mask: u8 = 0x07;
pub const mctrcntr_start: u16 = 0xA500 | 0x0001;
pub const msuinitr_full_init: u16 = 0xAA01;
pub const msuinitr_suinit: u16 = 0x0001;
pub const pe_spin_limit: u32 = 0x00010000;
pub const max_mrcfreq_mhz: u16 = 250;
pub const max_mrefreq_mhz: u8 = 125;

pub fn reg(off: usize) usize {
    return mram_base + off;
}

/// MRCEECC: key 0xC000 plus ECCEN (bit 0).
pub fn encoderWord(enable: bool) u16 {
    return 0xC000 | @as(u16, @intFromBool(enable));
}

/// MRCDECC: key 0x8C00 plus DECECEN (bit 1).
pub fn decoderWord(enable: bool) u16 {
    return 0x8C00 | @as(u16, @intFromBool(enable)) << 1;
}

pub const Status = struct { busy: u8, done: u8, err: u8 };

/// MCTRSTATR: BUSY bit 0, DONE bit 2, any of ERR bits 7:3.
pub fn status(v: u16) Status {
    return .{
        .busy = @intFromBool(v & 0x0001 != 0),
        .done = @intFromBool(v & 0x0004 != 0),
        .err = @intFromBool(v & 0x00F8 != 0),
    };
}

/// Kick MREZC and poll MREZS.WHUKEXE; false when it never clears.
pub fn zeroize(hw: anytype, spins: u32) bool {
    hw.write16(reg(off_mrezc), mrezc_full_zero);
    var i: u32 = 0;
    while (i < spins) : (i += 1) {
        if (hw.read8(reg(off_mrezs)) & mrezs_whukexe == 0) return true;
    }
    return false;
}

/// MRCFREQ / MREFREQ bounds: code MRAM up to 250 MHz, extra MRAM up to 125.
pub fn freqOk(mrc_mhz: u16, mre_mhz: u8) bool {
    return mrc_mhz <= max_mrcfreq_mhz and mre_mhz <= max_mrefreq_mhz;
}

/// MRCFREQ: key 0x1E in bits 31:24, MHz below.
pub fn mrcfreqWord(mhz: u16) u32 {
    return @as(u32, 0x1E) << 24 | mhz;
}

/// MREFREQ: key 0xE1 in bits 31:24, MHz below.
pub fn mrefreqWord(mhz: u8) u32 {
    return @as(u32, 0xE1) << 24 | mhz;
}
