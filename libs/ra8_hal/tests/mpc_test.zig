//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const mpc = @import("mpc");
const Code = mpc.Code;

/// Host RAM standing in for PFS (0x000..0x3C0) and PMISC (0x500..0x518).
const Fake = struct {
    words: [0x518 / 4]u32 align(4) = @splat(0),

    fn block(f: *Fake) mpc.Block {
        return .{ .base = @intFromPtr(&f.words) };
    }

    fn pfs(f: *Fake, port: usize, pin: usize) u32 {
        return f.words[port * mpc.pin_count + pin];
    }

    fn byte(f: *Fake, off: usize) u8 {
        const bytes: [*]const u8 = @ptrCast(&f.words);
        return bytes[off];
    }
};

test "bounds checks reject port 15 and pin 16 without touching PWPR" {
    var f = Fake{};
    const b = f.block();
    try std.testing.expectEqual(Code.gpio_invalid_port, b.setAnalog(15, 0));
    try std.testing.expectEqual(Code.gpio_invalid_pin, b.setIrq(14, 16));
    var v: u32 = 0;
    try std.testing.expectEqual(Code.gpio_invalid_pin, b.readPfs(0, 16, &v));
    try std.testing.expectEqual(@as(u8, 0), f.byte(mpc.pwpr_off));
}

test "routePeripheral leaves PSEL | PMR and both write-protects locked" {
    var f = Fake{};
    const b = f.block();
    try std.testing.expectEqual(Code.ok, b.routePeripheral(14, 15, 0x1F));
    try std.testing.expectEqual(@as(u32, 0x1F01_0000), f.pfs(14, 15));
    try std.testing.expectEqual(mpc.pwpr_b0wi, f.byte(mpc.pwpr_off));
    try std.testing.expectEqual(mpc.pwpr_b0wi, f.byte(mpc.pwprs_off));
    try std.testing.expectEqual(@as(u32, 0x0400_0000), mpc.pselBits(0x24));
}

test "gpio, analog and irq replace the whole register" {
    var f = Fake{};
    const b = f.block();
    f.words[3 * 16 + 2] = 0xFFFF_FFFF;
    try std.testing.expectEqual(Code.ok, b.setGpio(3, 2, true));
    try std.testing.expectEqual(mpc.mask_pdr, f.pfs(3, 2));
    try std.testing.expectEqual(Code.ok, b.setGpio(3, 2, false));
    try std.testing.expectEqual(@as(u32, 0), f.pfs(3, 2));
    try std.testing.expectEqual(Code.ok, b.setAnalog(3, 2));
    try std.testing.expectEqual(mpc.mask_asel, f.pfs(3, 2));
    try std.testing.expectEqual(Code.ok, b.setIrq(3, 2));
    try std.testing.expectEqual(mpc.mask_isel, f.pfs(3, 2));
}

test "pull and open-drain read-modify-write their own bit" {
    var f = Fake{};
    const b = f.block();
    f.words[5] = 0x0101_0004;
    try std.testing.expectEqual(Code.ok, b.setPull(0, 5, true));
    try std.testing.expectEqual(@as(u32, 0x0101_0014), f.pfs(0, 5));
    try std.testing.expectEqual(Code.ok, b.setOpenDrain(0, 5, true));
    try std.testing.expectEqual(@as(u32, 0x0101_0054), f.pfs(0, 5));
    try std.testing.expectEqual(Code.ok, b.setPull(0, 5, false));
    try std.testing.expectEqual(Code.ok, b.setOpenDrain(0, 5, false));
    try std.testing.expectEqual(@as(u32, 0x0101_0004), f.pfs(0, 5));
}

test "readPfs returns the register and pmn addresses step 4 bytes per pin" {
    var f = Fake{};
    const b = f.block();
    f.words[1 * 16 + 7] = 0x1234_5678;
    var v: u32 = 0;
    try std.testing.expectEqual(Code.ok, b.readPfs(1, 7, &v));
    try std.testing.expectEqual(@as(u32, 0x1234_5678), v);
    const real = mpc.Block{};
    try std.testing.expectEqual(@as(usize, 0x4040_0BBC), @intFromPtr(real.pmn(14, 15)));
}
