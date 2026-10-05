//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const s = @import("i3c_i2c_scan");

const Op = enum { clear, start, send, stop };

const Fake = struct {
    ops: [8]Op = undefined,
    n: usize = 0,
    sent: u8 = 0,
    send_rc: u16 = 0,

    pub fn push(self: *Fake, op: Op) void {
        self.ops[self.n] = op;
        self.n += 1;
    }
    pub fn clearBst(self: *Fake) void {
        self.push(.clear);
    }
    pub fn start(self: *Fake) void {
        self.push(.start);
    }
    pub fn stop(self: *Fake) void {
        self.push(.stop);
    }
    pub fn sendAddress(self: *Fake, byte: u8) u16 {
        self.push(.send);
        self.sent = byte;
        return self.send_rc;
    }
};

fn expectOps(f: *const Fake, want: []const Op) !void {
    try std.testing.expectEqualSlices(Op, want, f.ops[0..f.n]);
}

test "TENDF reports acked" {
    var f = Fake{};
    var bst: u32 = s.bst_tendf;
    var acked = false;
    try std.testing.expectEqual(@as(u16, 0), s.run(&f, &bst, 0x50, &acked));
    try std.testing.expect(acked);
    try std.testing.expectEqual(@as(u8, 0xA0), f.sent);
    try expectOps(&f, &.{ .clear, .start, .send, .stop, .clear });
}

test "NACKDF reports not acked" {
    var f = Fake{};
    var bst: u32 = s.bst_tendf | s.bst_nackdf;
    var acked = true;
    try std.testing.expectEqual(@as(u16, 0), s.run(&f, &bst, 0x10, &acked));
    try std.testing.expect(!acked);
}

test "no flag times out with acked false" {
    var f = Fake{};
    var bst: u32 = 0;
    var acked = true;
    try std.testing.expectEqual(@as(u16, 0x203), s.run(&f, &bst, 0x10, &acked));
    try std.testing.expect(!acked);
    try expectOps(&f, &.{ .clear, .start, .send, .stop, .clear });
}

test "address failure stops without clearing" {
    var f = Fake{ .send_rc = 0x203 };
    var bst: u32 = s.bst_tendf;
    var acked = true;
    try std.testing.expectEqual(@as(u16, 0x203), s.run(&f, &bst, 0x10, &acked));
    try std.testing.expect(!acked);
    try expectOps(&f, &.{ .clear, .start, .send, .stop });
}

test "address byte keeps the C truncation" {
    try std.testing.expectEqual(@as(u8, 0xFE), s.addressByte(0x7F));
    try std.testing.expectEqual(@as(u8, 0x00), s.addressByte(0x80));
}
