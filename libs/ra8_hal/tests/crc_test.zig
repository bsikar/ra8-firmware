//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/crc.zig on a fake CRC register block.

const std = @import("std");
const crc = @import("crc");

const Fake = struct {
    regs: [crc.block_size / 4]u32 align(4) = [_]u32{0} ** (crc.block_size / 4),

    fn block(f: *Fake) crc.Block {
        return .{ .base = @intFromPtr(&f.regs) };
    }

    fn bytes(f: *Fake) *[crc.block_size]u8 {
        return @ptrCast(&f.regs);
    }
};

test "select writes GPS with DORCLR in one store and snoopOff clears CRCCR1" {
    var f = Fake{};
    f.bytes()[crc.off_crccr1] = 0xC0;
    const b = f.block();
    b.select(crc.poly_32c_rev);
    b.snoopOff();
    try std.testing.expectEqual(@as(u8, 0x85), f.bytes()[crc.off_crccr0]);
    try std.testing.expectEqual(@as(u8, 0), f.bytes()[crc.off_crccr1]);
}

test "reset keeps GPS and LMS and sets DORCLR" {
    var f = Fake{};
    f.bytes()[crc.off_crccr0] = 0x42;
    f.block().reset();
    try std.testing.expectEqual(@as(u8, 0xC2), f.bytes()[crc.off_crccr0]);
}

test "clear and stop zero the control registers" {
    var f = Fake{};
    f.bytes()[crc.off_crccr0] = 0x83;
    f.bytes()[crc.off_crccr1] = 0x40;
    f.block().stop();
    try std.testing.expectEqual(@as(u8, 0), f.bytes()[crc.off_crccr0]);
    try std.testing.expectEqual(@as(u8, 0x40), f.bytes()[crc.off_crccr1]);
    f.block().clear();
    try std.testing.expectEqual(@as(u8, 0), f.bytes()[crc.off_crccr1]);
}

test "32-bit polynomials seed CRCDOR, feed whole LE words and xor the result" {
    var f = Fake{};
    f.bytes()[crc.off_crccr0] = crc.poly_32_ieee802_3;
    const data = [_]u8{ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xAA };
    const out = f.block().compute(&data);
    // The fake has no CRC engine: CRCDOR keeps the seed, so out is 0 and
    // CRCDIR holds the last whole word; the trailing 0xAA is not fed.
    try std.testing.expectEqual(@as(u32, 0), out);
    try std.testing.expectEqual(@as(u32, 0x0807_0605), f.regs[crc.off_crcdir / 4]);
    try std.testing.expectEqual(crc.seed32, f.regs[crc.off_crcdor / 4]);
}

test "8 and 16-bit polynomials feed bytes and read CRCDOR unmodified" {
    var f = Fake{};
    f.bytes()[crc.off_crccr0] = 0x80 | 2;
    f.regs[crc.off_crcdor / 4] = 0x1234;
    f.regs[crc.off_crcdir / 4] = 0xFFFF_FF00;
    const out = f.block().compute(&[_]u8{ 0x11, 0x22, 0x33 });
    try std.testing.expectEqual(@as(u32, 0x1234), out);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FF33), f.regs[crc.off_crcdir / 4]);
}

test "is32Bit picks only CRC-32 and CRC-32C" {
    try std.testing.expect(crc.is32Bit(4));
    try std.testing.expect(crc.is32Bit(5));
    for ([_]u8{ 0, 1, 2, 3, 6, 7 }) |p| try std.testing.expect(!crc.is32Bit(p));
}
