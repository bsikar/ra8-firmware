//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/flash_ctl.zig (RA8FW-806).

const std = @import("std");
const ctl = @import("flash_ctl");

const expectEqual = std.testing.expectEqual;

const Fake = struct {
    mrezc: u16 = 0,
    busy_polls: u32 = 0,
    polls: u32 = 0,

    pub fn write16(self: *Fake, a: usize, v: u16) void {
        std.debug.assert(a == ctl.reg(ctl.off_mrezc));
        self.mrezc = v;
    }
    pub fn read8(self: *Fake, a: usize) u8 {
        std.debug.assert(a == ctl.reg(ctl.off_mrezs));
        self.polls += 1;
        return if (self.polls <= self.busy_polls) ctl.mrezs_whukexe else 0;
    }
};

test "ECC control words carry the HUM keys" {
    try expectEqual(@as(u16, 0xC001), ctl.encoderWord(true));
    try expectEqual(@as(u16, 0xC000), ctl.encoderWord(false));
    try expectEqual(@as(u16, 0x8C02), ctl.decoderWord(true));
    try expectEqual(@as(u16, 0x8C00), ctl.decoderWord(false));
}

test "register addresses sit on the secure MRMS base" {
    try expectEqual(@as(usize, 0x4013C100), ctl.reg(ctl.off_msar));
    try expectEqual(@as(usize, 0x4013F804), ctl.reg(ctl.off_mrceecc));
    try expectEqual(@as(usize, 0x4013E06C), ctl.reg(ctl.off_mctrstatr));
    try expectEqual(@as(u16, 0xA501), ctl.mctrcntr_start);
}

test "update status decodes busy, done and any error bit" {
    const idle = ctl.status(0);
    try expectEqual(@as(u8, 0), idle.busy + idle.done + idle.err);
    const all = ctl.status(0x00FD);
    try expectEqual(@as(u8, 1), all.busy);
    try expectEqual(@as(u8, 1), all.done);
    try expectEqual(@as(u8, 1), all.err);
    try expectEqual(@as(u8, 1), ctl.status(0x0008).err);
    try expectEqual(@as(u8, 0), ctl.status(0x0002).busy);
}

test "zeroize kicks MREZC and returns once WHUKEXE clears" {
    var hw = Fake{ .busy_polls = 3 };
    try expectEqual(true, ctl.zeroize(&hw, 10));
    try expectEqual(@as(u16, 0xA505), hw.mrezc);
    try expectEqual(@as(u32, 4), hw.polls);
}

test "zeroize times out when WHUKEXE never clears" {
    var hw = Fake{ .busy_polls = 100 };
    try expectEqual(false, ctl.zeroize(&hw, 5));
    try expectEqual(@as(u32, 5), hw.polls);
}

test "clock-frequency bounds are 250 MHz code and 125 MHz extra" {
    try expectEqual(true, ctl.freqOk(250, 125));
    try expectEqual(true, ctl.freqOk(0, 0));
    try expectEqual(false, ctl.freqOk(251, 125));
    try expectEqual(false, ctl.freqOk(250, 126));
}

test "MRCFREQ and MREFREQ carry their keys in bits 31:24" {
    try expectEqual(@as(u32, 0x1E0000FA), ctl.mrcfreqWord(250));
    try expectEqual(@as(u32, 0xE1000064), ctl.mrefreqWord(100));
    try expectEqual(@as(usize, 0x4013C004), ctl.reg(ctl.off_mrcfreq));
    try expectEqual(@as(usize, 0x4013C008), ctl.reg(ctl.off_mrefreq));
}

test "MSUINITR kick word, SUINIT bit and address" {
    try expectEqual(@as(u16, 0xAA01), ctl.msuinitr_full_init);
    try expectEqual(@as(u16, 0x0001), ctl.msuinitr_suinit);
    try expectEqual(@as(usize, 0x4013E08C), ctl.reg(ctl.off_msuinitr));
    try expectEqual(@as(u32, 0x10000), ctl.pe_spin_limit);
}
