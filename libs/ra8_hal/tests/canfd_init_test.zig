//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/canfd_init.zig (RA8FW-864).

const std = @import("std");
const ini = @import("canfd_init");

const Fake = struct {
    srdy_fail: ?bool = null,
    mstp_rc: u16 = 0,
    open_rc: u16 = 0,
    ckcr: [4]u8 = undefined,
    nck: usize = 0,
    trail: [64]u8 = undefined,
    n: usize = 0,

    fn mark(self: *Fake, c: u8) void {
        self.trail[self.n] = c;
        self.n += 1;
    }
    pub fn prcr(self: *Fake, v: u16) void {
        self.mark(if (v == ini.prcr_unlock_cgc) 'U' else 'L');
    }
    pub fn writeDivcr(self: *Fake, v: u8) void {
        std.debug.assert(v == 0);
        self.mark('d');
    }
    pub fn writeCkcr(self: *Fake, v: u8) void {
        self.ckcr[self.nck] = v;
        self.nck += 1;
        self.mark('k');
    }
    pub fn waitSrdy(self: *Fake, set: bool) bool {
        self.mark(if (set) 'S' else 's');
        return self.srdy_fail != set;
    }
    pub fn mstpEnable(self: *Fake, id: u16) u16 {
        _ = id;
        self.mark('M');
        return self.mstp_rc;
    }
    pub fn waitGramInit(self: *Fake, _: u8) void {
        self.mark('G');
    }
    pub fn openChannel(self: *Fake, _: u8) u16 {
        self.mark('O');
        return self.open_rc;
    }
    pub fn channelReset(self: *Fake, _: u8) u16 {
        self.mark('R');
        return 0;
    }
    pub fn info(self: *Fake, _: [*:0]const u8) void {
        self.mark('i');
    }
    pub fn infoVal(self: *Fake, _: [*:0]const u8, _: u32) void {
        self.mark('v');
    }
    pub fn err(self: *Fake, _: [*:0]const u8) void {
        self.mark('e');
    }
    pub fn fail(self: *Fake, _: [*:0]const u8, _: u16) void {
        self.mark('f');
    }
};

test "init runs the clock once, then MSTP, GRAMINIT and open" {
    var f = Fake{};
    var done = false;
    try std.testing.expectEqual(@as(u16, 0), ini.init(&f, 1, &done));
    try std.testing.expectEqualStrings("UdkSksLiMGOv", f.trail[0..f.n]);
    try std.testing.expectEqual(@as(u8, 0x41), f.ckcr[0]);
    try std.testing.expectEqual(@as(u8, 0x01), f.ckcr[1]);
    try std.testing.expect(done);
    f.n = 0;
    try std.testing.expectEqual(@as(u16, 0), ini.init(&f, 0, &done));
    try std.testing.expectEqualStrings("MGOv", f.trail[0..f.n]);
}

test "an SRDY timeout re-locks PRCR and leaves the clock un-inited" {
    var f = Fake{ .srdy_fail = true };
    var done = false;
    try std.testing.expectEqual(@as(u16, 0x203), ini.init(&f, 0, &done));
    try std.testing.expectEqualStrings("UdkSeL", f.trail[0..f.n]);
    try std.testing.expect(!done);
    var g = Fake{ .srdy_fail = false };
    try std.testing.expectEqual(@as(u16, 0x203), ini.clockInit(&g, &done));
    try std.testing.expectEqualStrings("UdkSkseL", g.trail[0..g.n]);
}

test "init reports a bad channel, MSTP and open failures" {
    var done = true;
    var f = Fake{};
    try std.testing.expectEqual(@as(u16, 0x504), ini.init(&f, 2, &done));
    try std.testing.expectEqualStrings("e", f.trail[0..f.n]);
    var g = Fake{ .mstp_rc = 0x204 };
    try std.testing.expectEqual(@as(u16, 0x204), ini.init(&g, 0, &done));
    try std.testing.expectEqualStrings("Mf", g.trail[0..g.n]);
    var h = Fake{ .open_rc = 0x203 };
    try std.testing.expectEqual(@as(u16, 0x203), ini.init(&h, 0, &done));
    try std.testing.expectEqualStrings("MGO", h.trail[0..h.n]);
}

test "deinit parks the channel in reset" {
    var f = Fake{};
    try std.testing.expectEqual(@as(u16, 0), ini.deinit(&f, 1));
    try std.testing.expectEqualStrings("R", f.trail[0..f.n]);
    try std.testing.expectEqual(@as(u16, 0x504), ini.deinit(&f, 2));
}
