//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const info = @import("mipi_csi_info");

const Fake = struct {
    regs: [0x80 / 4]u32 = @splat(0),
    errs: u8 = 0,

    pub fn read32(f: *Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
    pub fn err(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
};

test "readReg returns RXST and MIST snapshots" {
    var f = Fake{};
    f.regs[info.off_rxst / 4] = 0x0002_0001;
    f.regs[info.off_mist / 4] = 0x0000_0110;
    var out: u32 = 0;
    try std.testing.expectEqual(info.ok, info.readReg(&f, info.off_rxst, &out));
    try std.testing.expectEqual(@as(u32, 0x0002_0001), out);
    try std.testing.expectEqual(info.ok, info.readReg(&f, info.off_mist, &out));
    try std.testing.expectEqual(@as(u32, 0x0000_0110), out);
}

test "null out pointers log and return null_ptr" {
    var f = Fake{};
    try std.testing.expectEqual(info.null_ptr, info.readReg(&f, info.off_rxst, null));
    try std.testing.expectEqual(info.null_ptr, info.getModuleInfo(&f, null));
    try std.testing.expectEqual(@as(u8, 2), f.errs);
}

test "module info decodes MCG fields and keeps raw" {
    var f = Fake{};
    f.regs[info.off_mcg / 4] = 0xAB10_0403;
    var out: info.ModuleInfo = undefined;
    try std.testing.expectEqual(info.ok, info.getModuleInfo(&f, &out));
    try std.testing.expectEqual(@as(u8, 3), out.version);
    try std.testing.expectEqual(@as(u8, 4), out.lanes_max);
    try std.testing.expectEqual(@as(u8, 0x10), out.fifo_stages);
    try std.testing.expectEqual(@as(u32, 0xAB10_0403), out.raw);
}

test "decodeMcg ignores bits outside the fields" {
    const m = info.decodeMcg(0xFF00_F0F0);
    try std.testing.expectEqual(@as(u8, 0), m.version);
    try std.testing.expectEqual(@as(u8, 0), m.lanes_max);
    try std.testing.expectEqual(@as(u8, 0), m.fifo_stages);
}

test "ModuleInfo matches the C layout" {
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(info.ModuleInfo));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(info.ModuleInfo, "fifo_stages"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(info.ModuleInfo, "raw"));
}
