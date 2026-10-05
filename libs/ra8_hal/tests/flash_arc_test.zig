//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/flash_arc.zig (RA8FW-802, ARC half of
//! ra8_flash_config.c).

const std = @import("std");
const arc = @import("flash_arc");

const expectEqual = std.testing.expectEqual;

/// OFS words: ARC_SEC (2 words) then ARC_NSEC (up to 64 words), plus ARCCS.
const Fake = struct {
    arccs: u16 = 0,
    sec: [2]u32 = .{ 0, 0 },
    nsec: [64]u32 = [_]u32{0} ** 64,

    pub fn read16(self: *const Fake, a: usize) u16 {
        std.debug.assert(a == arc.arccs_addr);
        return self.arccs;
    }
    pub fn read32(self: *const Fake, a: usize) u32 {
        if (a >= arc.nsec_addr) return self.nsec[(a - arc.nsec_addr) / 4];
        return self.sec[(a - arc.sec_addr) / 4];
    }
};

test "MCNTSELR follows the HUM map and rejects out-of-range ids" {
    try expectEqual(@as(u8, 1), arc.mcntselr(arc.arc_sec));
    try expectEqual(@as(u8, 2), arc.mcntselr(arc.arc_oembl));
    try expectEqual(@as(u8, 4), arc.mcntselr(2));
    try expectEqual(@as(u8, 5), arc.mcntselr(3));
    try expectEqual(@as(u8, 6), arc.mcntselr(4));
    try expectEqual(@as(u8, 7), arc.mcntselr(5));
    try expectEqual(@as(u8, 0), arc.mcntselr(arc.arc_count));
}

test "maximum count per id and ARCNS mode" {
    try expectEqual(@as(u32, 64), arc.maxCount(arc.arc_sec, 1));
    try expectEqual(@as(u32, 64), arc.maxCount(arc.arc_oembl, 1));
    try expectEqual(@as(u32, 256), arc.maxCount(2, 1));
    try expectEqual(@as(u32, 256), arc.maxCount(5, 0xFFFD));
    try expectEqual(@as(u32, 64), arc.maxCount(2, 0));
    try expectEqual(@as(u32, 64), arc.maxCount(3, 3));
}

test "NSEC spans: 16 words per slot single, 2 words per slot multiple" {
    const single = arc.nsecSpan(4, 1);
    try expectEqual(@as(u32, 32), single.first);
    try expectEqual(@as(u32, 16), single.words);
    const multi = arc.nsecSpan(5, 2);
    try expectEqual(@as(u32, 6), multi.first);
    try expectEqual(@as(u32, 2), multi.words);
    try expectEqual(@as(u32, 0), arc.nsecSpan(2, 2).first);
    try expectEqual(@as(u32, 2), arc.nsecSpan(3, 2).first);
}

test "SEC count is the popcount of its two words only" {
    var hw = Fake{};
    hw.sec = .{ 0xFFFF_FFFF, 0x0000_0007 };
    hw.nsec[0] = 0xFFFF_FFFF; // next OFS word must not be counted
    try expectEqual(@as(u32, 35), arc.ofsCount(&hw, arc.arc_sec));
}

test "NSEC count reads only its own slot" {
    var hw = Fake{ .arccs = 2 };
    hw.nsec[2] = 0x0000_000F;
    hw.nsec[3] = 0x8000_0001;
    hw.nsec[4] = 0xFFFF_FFFF;
    try expectEqual(@as(u32, 6), arc.ofsCount(&hw, 3));
    try expectEqual(@as(u32, 0), arc.ofsCount(&hw, 2));
    hw.arccs = 1;
    try expectEqual(@as(u32, 6 + 32), arc.ofsCount(&hw, 2));
}

test "popWords sums n consecutive words" {
    var hw = Fake{};
    hw.nsec[10] = 0x3;
    hw.nsec[11] = 0x30;
    try expectEqual(@as(u32, 4), arc.popWords(&hw, arc.nsec_addr + 40, 2));
    try expectEqual(@as(u32, 2), arc.popWords(&hw, arc.nsec_addr + 40, 1));
    try expectEqual(@as(u32, 0), arc.popWords(&hw, arc.nsec_addr + 40, 0));
}
