//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/flash_cfgset.zig (RA8FW-808).

const std = @import("std");
const cfg = @import("flash_cfgset");
const expectEqual = std.testing.expectEqual;

test "region accepts the OFS and extra-MRAM windows only" {
    try expectEqual(cfg.Region.ofs, cfg.region(0x02C9F000));
    try expectEqual(cfg.Region.ofs, cfg.region(0x02C9FFFF));
    try expectEqual(cfg.Region.none, cfg.region(0x02CA0000));
    try expectEqual(cfg.Region.none, cfg.region(0x02C9EFFF));
    try expectEqual(cfg.Region.extra, cfg.region(0x02E07600));
    try expectEqual(cfg.Region.extra, cfg.region(0x02E179FF));
    try expectEqual(cfg.Region.none, cfg.region(0x02E17A00));
    try expectEqual(cfg.Region.none, cfg.region(0xFFFFFFFF));
}

test "opener is Program for extra MRAM and Config Set for OFS" {
    try expectEqual(@as(u8, 0xE8), cfg.opener(.extra));
    try expectEqual(@as(u8, 0x40), cfg.opener(.ofs));
}

test "MSUACR word carries key 0x66 and the alternate bit" {
    try expectEqual(@as(u16, 0x6600), cfg.msuacrWord(cfg.startup_default));
    try expectEqual(@as(u16, 0x6601), cfg.msuacrWord(cfg.startup_alternate));
    try expectEqual(@as(u16, 0x6600), cfg.msuacrWord(cfg.startup_max));
    try expectEqual(@as(usize, 0x4013E0E8), cfg.reg(cfg.off_msuacr));
}

test "BTFLG configuration set is all ones except word 3" {
    const d = cfg.startupWords(cfg.startup_default);
    const a = cfg.startupWords(cfg.startup_alternate);
    try expectEqual(@as(u16, 0x9FFF), d[3]);
    try expectEqual(@as(u16, 0x1FFF), a[3]);
    for (d, 0..) |w, i| if (i != 3) try expectEqual(@as(u16, 0xFFFF), w);
    try expectEqual(@as(u32, 0x02C9F070), cfg.startup_addr);
}

test "MSUASMON decodes BTFLG bit 31 and FSPR bit 15" {
    const both = cfg.startupFlags(0x80008000);
    try expectEqual(@as(u8, 1), both.btflg);
    try expectEqual(@as(u8, 1), both.fspr);
    const none = cfg.startupFlags(0x7FFF7FFF);
    try expectEqual(@as(u8, 0), none.btflg);
    try expectEqual(@as(u8, 0), none.fspr);
}
