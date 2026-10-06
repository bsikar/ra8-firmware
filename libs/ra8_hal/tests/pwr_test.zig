//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const pwr = @import("pwr");

/// Host RAM standing in for the SYSTEM block up to WUPEN1.
const Fake = struct {
    words: [(pwr.off_wupen1 / 4) + 1]u32 = @splat(0),

    fn block(f: *Fake) pwr.Block {
        return .{ .base = @intFromPtr(&f.words) };
    }

    fn at(f: *Fake, off: usize) u32 {
        return f.words[off / 4];
    }
};

test "decode splits register and bit and rejects out-of-range sources" {
    const irq3 = pwr.Wake.decode(3).?;
    try std.testing.expectEqual(@as(u1, 0), irq3.reg);
    try std.testing.expectEqual(@as(u32, 1 << 3), irq3.mask());
    const agt1 = pwr.Wake.decode((1 << 8) | 1).?;
    try std.testing.expectEqual(@as(u1, 1), agt1.reg);
    try std.testing.expectEqual(@as(u5, 1), agt1.bit);
    try std.testing.expect(pwr.Wake.decode(2 << 8) == null);
    try std.testing.expect(pwr.Wake.decode(32) == null);
    try std.testing.expect(pwr.Wake.decode(0xFFFF) == null);
}

test "enable and disable touch only their bit in the right WUPEN" {
    var f = Fake{};
    f.words[pwr.off_wupen0 / 4] = 0x8000_0000;
    const b = f.block();
    b.enable(pwr.Wake.decode(24).?);
    try std.testing.expectEqual(@as(u32, 0x8100_0000), f.at(pwr.off_wupen0));
    b.enable(pwr.Wake.decode((1 << 8) | 2).?);
    try std.testing.expectEqual(@as(u32, 0x4), f.at(pwr.off_wupen1));
    b.disable(pwr.Wake.decode(24).?);
    try std.testing.expectEqual(@as(u32, 0x8000_0000), f.at(pwr.off_wupen0));
}

test "isEnabled reads back the bit" {
    var f = Fake{};
    const b = f.block();
    const w = pwr.Wake.decode(16).?;
    try std.testing.expect(!b.isEnabled(w));
    b.enable(w);
    try std.testing.expect(b.isEnabled(w));
}

test "anyArmed is false until either register has a bit" {
    var f = Fake{};
    const b = f.block();
    try std.testing.expect(!b.anyArmed());
    f.words[pwr.off_wupen1 / 4] = 1;
    try std.testing.expect(b.anyArmed());
}

test "waitForInterrupt is a no-op in a host test binary" {
    try std.testing.expect(!pwr.on_target);
    pwr.waitForInterrupt();
}
