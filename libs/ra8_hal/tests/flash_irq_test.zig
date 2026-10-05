//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MRAM IRQ, range and status logic (RA8FW-758).

const std = @import("std");
const fi = @import("flash_irq");

const Regs = struct {
    mem: [0x3100]u8 = [_]u8{0} ** 0x3100,
    pub fn read8(self: *Regs, o: u16) u8 {
        return self.mem[o];
    }
    pub fn write8(self: *Regs, o: u16, v: u8) void {
        self.mem[o] = v;
    }
    pub fn read32(self: *Regs, o: u16) u32 {
        return std.mem.readInt(u32, self.mem[o..][0..4], .little);
    }
    pub fn put32(self: *Regs, o: u16, v: u32) void {
        std.mem.writeInt(u32, self.mem[o..][0..4], v, .little);
    }
};

const Sink = struct {
    srcs: [8]u8 = undefined,
    addrs: [8]u32 = undefined,
    words: [8]u32 = undefined,
    n: usize = 0,
    pub fn deliver(self: *Sink, s: u8, fault_addr: u32, status_word: u32) void {
        self.srcs[self.n] = s;
        self.addrs[self.n] = fault_addr;
        self.words[self.n] = status_word;
        self.n += 1;
    }
};

test "ECC and extra error sources toggle one bit and keep the rest" {
    var r = Regs{};
    r.mem[fi.off.mrcraeint] = 0x40;
    try std.testing.expect(fi.setIrq(&r, fi.src.code_ecc_ted, true));
    try std.testing.expect(fi.setIrq(&r, fi.src.code_ecc_dec, true));
    try std.testing.expectEqual(@as(u8, 0x43), r.mem[fi.off.mrcraeint]);
    try std.testing.expect(fi.setIrq(&r, fi.src.code_ecc_ted, false));
    try std.testing.expectEqual(@as(u8, 0x41), r.mem[fi.off.mrcraeint]);
    try std.testing.expect(fi.setIrq(&r, fi.src.extra_ecc_ted, true));
    try std.testing.expectEqual(@as(u8, 0x02), r.mem[fi.off.mreraint]);
    try std.testing.expect(fi.setIrq(&r, fi.src.extra_err, true));
    try std.testing.expect(fi.setIrq(&r, fi.src.extra_cmdlk, true));
    try std.testing.expectEqual(@as(u8, 0x18), r.mem[fi.off.mpaeint]);
}

test "program error and ready enables are whole-register writes" {
    var r = Regs{};
    r.mem[fi.off.mrcpaeint] = 0x7F;
    try std.testing.expect(fi.setIrq(&r, fi.src.program_err, true));
    try std.testing.expectEqual(@as(u8, 0x80), r.mem[fi.off.mrcpaeint]);
    try std.testing.expect(fi.setIrq(&r, fi.src.extra_ready, true));
    try std.testing.expectEqual(@as(u8, 0x01), r.mem[fi.off.mrdyie]);
    try std.testing.expect(fi.setIrq(&r, fi.src.extra_ready, false));
    try std.testing.expectEqual(@as(u8, 0x00), r.mem[fi.off.mrdyie]);
    try std.testing.expect(!fi.setIrq(&r, fi.src.count, true));
}

test "dispatch delivers in order and clears what it handled" {
    var r = Regs{};
    var s = Sink{};
    r.mem[fi.off.mrcraes] = 0x03;
    r.put32(fi.off.mrcrtea, 0x0200_1000);
    r.put32(fi.off.mrcrdea, 0x0200_2000);
    r.mem[fi.off.mreraes] = 0x01;
    r.put32(fi.off.mrerdea, 0x02E0_7700);
    r.mem[fi.off.mrcps] = 0x81;
    r.put32(fi.off.mrcpea, 0x0200_0040);
    r.mem[fi.off.mastat] = 0x18;
    r.put32(fi.off.mstatr, 0x8000);
    try std.testing.expectEqual(@as(u32, 7), fi.dispatch(&r, &s, true));
    const want = [_]u8{ 0, 1, 3, 4, 5, 6, 7 };
    try std.testing.expectEqualSlices(u8, &want, s.srcs[0..7]);
    try std.testing.expectEqual(@as(u32, 0x0200_1000), s.addrs[0]);
    try std.testing.expectEqual(@as(u32, 0x0200_2000), s.addrs[1]);
    try std.testing.expectEqual(@as(u32, 0x02E0_7700), s.addrs[2]);
    try std.testing.expectEqual(@as(u32, 0x81), s.words[3]);
    try std.testing.expectEqual(@as(u32, 0), s.addrs[4]);
    try std.testing.expectEqual(@as(u32, 0x8000), s.words[6]);
    try std.testing.expectEqual(@as(u8, 0), r.mem[fi.off.mrcraes]);
    try std.testing.expectEqual(@as(u8, 0), r.mem[fi.off.mreraes]);
    try std.testing.expectEqual(@as(u8, 0), r.mem[fi.off.mrcps]);
}

test "dispatch with nothing pending delivers nothing" {
    var r = Regs{};
    var s = Sink{};
    try std.testing.expectEqual(@as(u32, 0), fi.dispatch(&r, &s, false));
    r.mem[fi.off.mrcps] = 0x80;
    try std.testing.expectEqual(@as(u32, 0), fi.dispatch(&r, &s, false));
    try std.testing.expectEqual(@as(u8, 0x80), r.mem[fi.off.mrcps]);
}

test "code range must be aligned and inside the 1 MiB code region" {
    try std.testing.expect(fi.codeRangeOk(0x0200_0000, 32));
    try std.testing.expect(fi.codeRangeOk(0x020F_FFE0, 32));
    try std.testing.expect(!fi.codeRangeOk(0x020F_FFE0, 64));
    try std.testing.expect(!fi.codeRangeOk(0x0200_0010, 32));
    try std.testing.expect(!fi.codeRangeOk(0x01FF_FFE0, 32));
}

test "blank checks accept code, extra and OFS only" {
    try std.testing.expect(fi.blankRegionOk(0x0200_0000, 1));
    try std.testing.expect(fi.blankRegionOk(0x02E0_7600, 0x10400));
    try std.testing.expect(!fi.blankRegionOk(0x02E0_7600, 0x10401));
    try std.testing.expect(fi.blankRegionOk(0x02C9_FFFF, 1));
    try std.testing.expect(!fi.blankRegionOk(0x02CA_0000, 1));
    try std.testing.expect(!fi.blankRegionOk(0x2000_0000, 4));
}

test "status decode" {
    const idle = fi.decodeStatus(0, 0, 0, 0, 1, 1);
    try std.testing.expect(!idle.programming_busy and !idle.erase_busy and !idle.sector_protected);
    const s = fi.decodeStatus(0x83, 0x10, 0, 0x0010_0000, 0, 1);
    try std.testing.expect(s.programming_busy and s.erase_busy and s.illegal_command);
    try std.testing.expect(s.voltage_error and s.sector_protected and s.program_error and s.ecc_error);
    try std.testing.expect(fi.decodeStatus(0, 0, 0x0080, 0x0080_0000, 1, 1).illegal_command);
    try std.testing.expect(fi.decodeStatus(0, 0, 0x0080, 0, 1, 1).erase_busy);
}
