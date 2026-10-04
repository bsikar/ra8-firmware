//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/usb_pvnd.zig.

const std = @import("std");
const pvnd = @import("usb_pvnd");
const codes = pvnd.codes;

/// Records each primitive call; `fail` sets the code a named call returns.
const Fake = struct {
    calls: [16][]const u8 = undefined,
    count: usize = 0,
    fail_name: []const u8 = "",
    fail_code: u16 = 0,
    last_mp: u16 = 0,
    last_pipe: u8 = 0,
    accept: ?bool = null,
    out_len: u16 = 7,
    logged: [4][*:0]const u8 = undefined,
    logs: usize = 0,

    fn hit(self: *Fake, name: []const u8) u16 {
        self.calls[self.count] = name;
        self.count += 1;
        if (std.mem.eql(u8, self.fail_name, name)) return self.fail_code;
        return 0;
    }
    pub fn deviceInit(self: *Fake, _: u8) u16 {
        return self.hit("init");
    }
    pub fn deviceDeinit(self: *Fake, _: u8) u16 {
        return self.hit("deinit");
    }
    pub fn deviceAttach(self: *Fake, _: u8, _: bool) u16 {
        return self.hit("attach");
    }
    pub fn configureEndpoint(self: *Fake, _: u8, pipe: u8, _: u8, _: u8, _: u8, mp: u16) u16 {
        self.last_mp = mp;
        self.last_pipe = pipe;
        return self.hit("ep");
    }
    pub fn queueIn(self: *Fake, _: u8, pipe: u8, _: [*]const u8, _: u16) u16 {
        self.last_pipe = pipe;
        return self.hit("in");
    }
    pub fn queueOut(self: *Fake, _: u8, pipe: u8, _: [*]u8, inout: *u16, _: bool) u16 {
        self.last_pipe = pipe;
        inout.* = self.out_len;
        return self.hit("out");
    }
    pub fn controlResponse(self: *Fake, _: u8, accept: bool) u16 {
        self.accept = accept;
        return self.hit("ctrl");
    }
    pub fn logError(self: *Fake, msg: [*:0]const u8) void {
        self.logged[self.logs] = msg;
        self.logs += 1;
    }
    pub fn logErrorVal(self: *Fake, msg: [*:0]const u8, _: u16) void {
        self.logError(msg);
    }
    pub fn logInfoVal(self: *Fake, msg: [*:0]const u8, _: u8) void {
        self.logError(msg);
    }
};

fn ready(fake: *Fake, speed: u8) pvnd.State {
    var s: pvnd.State = .{};
    std.debug.assert(s.init(fake, speed) == codes.ok);
    return s;
}

test "init rejects an unknown speed before touching the controller" {
    var fake: Fake = .{};
    var s: pvnd.State = .{};
    try std.testing.expectEqual(codes.invalid_arg, s.init(&fake, 2));
    try std.testing.expectEqual(@as(usize, 0), fake.count);
}

test "init maps a controller failure to hw_init_failed and stays closed" {
    var fake: Fake = .{ .fail_name = "init", .fail_code = 0x202 };
    var s: pvnd.State = .{};
    try std.testing.expectEqual(codes.hw_init_failed, s.init(&fake, pvnd.speed_fs));
    try std.testing.expect(!s.initialized);
    try std.testing.expectEqualStrings("ra8_usb_device_init failed", std.mem.span(fake.logged[0]));
}

test "init configures both bulk pipes at the speed's packet size" {
    var fake: Fake = .{};
    const s = ready(&fake, pvnd.speed_hs);
    try std.testing.expect(s.initialized);
    try std.testing.expectEqual(@as(usize, 3), fake.count);
    try std.testing.expectEqual(@as(u16, 512), fake.last_mp);
    try std.testing.expectEqual(pvnd.pipe_bulk_out, fake.last_pipe);
    try std.testing.expectEqualStrings("device-Vendor ready", std.mem.span(fake.logged[0]));
    try std.testing.expectEqual(@as(u16, 64), pvnd.bulkMaxPacket(pvnd.speed_fs));
}

test "close detaches, returns the deinit code and keeps desc_len" {
    var fake: Fake = .{ .fail_name = "deinit", .fail_code = 0x104 };
    var s = ready(&fake, pvnd.speed_fs);
    const blob = [_]u8{ 1, 2, 3 };
    try std.testing.expectEqual(codes.ok, s.setDescriptors(&fake, &blob, 3));
    try std.testing.expectEqual(@as(u16, 0x104), s.close(&fake));
    try std.testing.expect(!s.initialized and s.desc == null);
    try std.testing.expectEqual(@as(u16, 3), s.desc_len);
    try std.testing.expectEqualStrings("attach", fake.calls[3]);
    try std.testing.expectEqual(codes.invalid_state, s.close(&fake));
}

test "set_descriptors checks state, then null, then length" {
    var fake: Fake = .{};
    var closed: pvnd.State = .{};
    try std.testing.expectEqual(codes.invalid_state, closed.setDescriptors(&fake, null, 0));
    var s = ready(&fake, pvnd.speed_fs);
    try std.testing.expectEqual(codes.null_ptr, s.setDescriptors(&fake, null, 4));
    try std.testing.expectEqualStrings("set_descriptors: desc", std.mem.span(fake.logged[1]));
    const blob = [_]u8{9};
    try std.testing.expectEqual(codes.invalid_arg, s.setDescriptors(&fake, &blob, 0));
}

test "send bounds the length by the bulk packet size" {
    var fake: Fake = .{};
    var closed: pvnd.State = .{};
    try std.testing.expectEqual(codes.invalid_state, closed.send(&fake, null, 1));
    const s = ready(&fake, pvnd.speed_fs);
    const data = [_]u8{0} ** 65;
    try std.testing.expectEqual(codes.null_ptr, s.send(&fake, null, 1));
    try std.testing.expectEqual(codes.invalid_arg, s.send(&fake, null, 0));
    try std.testing.expectEqual(codes.invalid_arg, s.send(&fake, &data, 65));
    try std.testing.expectEqual(codes.ok, s.send(&fake, &data, 64));
    try std.testing.expectEqual(pvnd.pipe_bulk_in, fake.last_pipe);
}

test "recv checks pointers before state and reports the length" {
    var fake: Fake = .{};
    var buf: [8]u8 = undefined;
    var got: u16 = 99;
    var closed: pvnd.State = .{};
    try std.testing.expectEqual(codes.null_ptr, closed.recv(&fake, null, 8, &got));
    try std.testing.expectEqual(codes.null_ptr, closed.recv(&fake, &buf, 8, null));
    try std.testing.expectEqualStrings("recv: got_len", std.mem.span(fake.logged[1]));
    try std.testing.expectEqual(codes.invalid_state, closed.recv(&fake, &buf, 8, &got));
    const s = ready(&fake, pvnd.speed_fs);
    try std.testing.expectEqual(codes.invalid_arg, s.recv(&fake, &buf, 0, &got));
    try std.testing.expectEqual(codes.ok, s.recv(&fake, &buf, 8, &got));
    try std.testing.expectEqual(@as(u16, 7), got);
    fake.fail_name = "out";
    fake.fail_code = 0x203;
    try std.testing.expectEqual(@as(u16, 0x203), s.recv(&fake, &buf, 8, &got));
    try std.testing.expectEqual(@as(u16, 0), got);
}

fn okHandler(ctx: ?*anyopaque, _: *const pvnd.Setup) callconv(.C) u16 {
    const n: *u32 = @ptrCast(@alignCast(ctx.?));
    n.* += 1;
    return 0;
}

fn failHandler(_: ?*anyopaque, _: *const pvnd.Setup) callconv(.C) u16 {
    return 0x107;
}

test "handle_setup filters the envelope and acks or stalls" {
    var fake: Fake = .{};
    var setup: pvnd.Setup = .{ .bm_request_type = 0x80, .b_request = 6, .w_value = 0, .w_index = 0, .w_length = 0 };
    var closed: pvnd.State = .{};
    try std.testing.expectEqual(codes.null_ptr, closed.handleSetup(&fake, null));
    try std.testing.expectEqual(codes.invalid_state, closed.handleSetup(&fake, &setup));
    try std.testing.expectEqual(codes.invalid_state, closed.attachSetupHandler(okHandler, null));
    var s = ready(&fake, pvnd.speed_fs);
    try std.testing.expectEqual(codes.not_supported, s.handleSetup(&fake, &setup));
    setup.bm_request_type = 0xC1;
    try std.testing.expectEqual(codes.ok, s.handleSetup(&fake, &setup));
    try std.testing.expectEqual(false, fake.accept.?);
    var hits: u32 = 0;
    try std.testing.expectEqual(codes.ok, s.attachSetupHandler(okHandler, &hits));
    try std.testing.expectEqual(codes.ok, s.handleSetup(&fake, &setup));
    try std.testing.expect(fake.accept.? and hits == 1);
    try std.testing.expectEqual(codes.ok, s.attachSetupHandler(failHandler, null));
    _ = s.handleSetup(&fake, &setup);
    try std.testing.expectEqual(false, fake.accept.?);
}

test "the vendor envelope set and the setup layout match the header" {
    for ([_]u8{ 0xC0, 0x40, 0xC1, 0x41, 0xC2, 0x42 }) |bm| try std.testing.expect(pvnd.isVendorEnvelope(bm));
    for ([_]u8{ 0x00, 0x80, 0x21, 0xA1, 0x43 }) |bm| try std.testing.expect(!pvnd.isVendorEnvelope(bm));
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(pvnd.Setup));
    try std.testing.expectEqual(@as(usize, 6), @offsetOf(pvnd.Setup, "w_length"));
}
