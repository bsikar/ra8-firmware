//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/usb_pprn.zig.

const std = @import("std");
const pprn = @import("usb_pprn");
const codes = pprn.codes;

/// Records each primitive call; `fail` sets the code a named call returns.
const Fake = struct {
    calls: [16][]const u8 = undefined,
    count: usize = 0,
    fail_name: []const u8 = "",
    fail_code: u16 = 0,
    pipes: [2]u8 = .{ 0, 0 },
    eps: [2]u8 = .{ 0, 0 },
    eps_seen: usize = 0,
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
    pub fn configureEndpoint(self: *Fake, _: u8, pipe: u8, ep: u8, _: u8, _: u8, mp: u16) u16 {
        self.pipes[self.eps_seen] = pipe;
        self.eps[self.eps_seen] = ep;
        self.eps_seen += 1;
        self.last_mp = mp;
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

fn ready(fake: *Fake, speed: u8) pprn.State {
    var s: pprn.State = .{};
    std.debug.assert(s.init(fake, speed) == codes.ok);
    return s;
}

fn okHandler(_: ?*anyopaque, _: *const pprn.Setup) callconv(.C) u16 {
    return codes.ok;
}

fn failHandler(ctx: ?*anyopaque, _: *const pprn.Setup) callconv(.C) u16 {
    const hits: *u8 = @ptrCast(ctx.?);
    hits.* += 1;
    return codes.invalid_arg;
}

test "init rejects an unknown speed and maps a controller failure" {
    var fake: Fake = .{};
    var s: pprn.State = .{};
    try std.testing.expectEqual(codes.invalid_arg, s.init(&fake, 2));
    try std.testing.expectEqual(@as(usize, 0), fake.count);
    fake = .{ .fail_name = "init", .fail_code = 0x202 };
    try std.testing.expectEqual(codes.hw_init_failed, s.init(&fake, pprn.speed_fs));
    try std.testing.expect(!s.initialized);
    try std.testing.expectEqualStrings("ra8_usb_device_init failed", std.mem.span(fake.logged[0]));
}

test "init configures EP1 OUT on PIPE3 then EP2 IN on PIPE4 and resets the shadow" {
    var fake: Fake = .{};
    const s = ready(&fake, pprn.speed_hs);
    try std.testing.expectEqualSlices(u8, &.{ 3, 4 }, &fake.pipes);
    try std.testing.expectEqualSlices(u8, &.{ 1, 2 }, &fake.eps);
    try std.testing.expectEqual(@as(u16, 512), fake.last_mp);
    try std.testing.expectEqual(@as(u8, 0x18), s.port_status);
    try std.testing.expectEqualStrings("device-Printer ready", std.mem.span(fake.logged[0]));
    try std.testing.expectEqual(@as(u16, 64), pprn.bulkMaxPacket(pprn.speed_fs));
}

test "close detaches, returns the deinit code and keeps the lengths" {
    var fake: Fake = .{ .fail_name = "deinit", .fail_code = 0x104 };
    var s = ready(&fake, pprn.speed_fs);
    const blob = [_]u8{ 1, 2, 3 };
    const id = [_]u8{ 0, 4 };
    try std.testing.expectEqual(codes.ok, s.setDescriptors(&fake, &blob, 3, &id, 2));
    try std.testing.expectEqual(@as(u16, 0x104), s.close(&fake));
    try std.testing.expect(!s.initialized and s.desc == null and s.device_id == null);
    try std.testing.expectEqual(@as(u16, 2), s.device_id_len);
    try std.testing.expectEqualStrings("attach", fake.calls[3]);
    try std.testing.expectEqual(codes.invalid_state, s.close(&fake));
}

test "set_descriptors checks state, null, length and the device-ID pair" {
    var fake: Fake = .{};
    var closed: pprn.State = .{};
    try std.testing.expectEqual(codes.invalid_state, closed.setDescriptors(&fake, null, 0, null, 0));
    var s = ready(&fake, pprn.speed_fs);
    try std.testing.expectEqual(codes.null_ptr, s.setDescriptors(&fake, null, 4, null, 0));
    try std.testing.expectEqualStrings("set_descriptors: desc", std.mem.span(fake.logged[1]));
    const blob = [_]u8{9};
    try std.testing.expectEqual(codes.invalid_arg, s.setDescriptors(&fake, &blob, 0, null, 0));
    try std.testing.expectEqual(codes.invalid_arg, s.setDescriptors(&fake, &blob, 1, &blob, 0));
    try std.testing.expectEqual(codes.invalid_arg, s.setDescriptors(&fake, &blob, 1, null, 1));
    try std.testing.expectEqual(codes.ok, s.setDescriptors(&fake, &blob, 1, null, 0));
    try std.testing.expectEqual(@as(u16, 1), s.desc_len);
}

test "recv checks pointers before state and reports the length" {
    var fake: Fake = .{};
    var buf: [8]u8 = undefined;
    var got: u16 = 99;
    var closed: pprn.State = .{};
    try std.testing.expectEqual(codes.null_ptr, closed.recv(&fake, null, 8, &got));
    try std.testing.expectEqual(codes.null_ptr, closed.recv(&fake, &buf, 8, null));
    try std.testing.expectEqualStrings("recv: got_len", std.mem.span(fake.logged[1]));
    try std.testing.expectEqual(codes.invalid_state, closed.recv(&fake, &buf, 8, &got));
    const s = ready(&fake, pprn.speed_fs);
    try std.testing.expectEqual(codes.invalid_arg, s.recv(&fake, &buf, 0, &got));
    try std.testing.expectEqual(codes.ok, s.recv(&fake, &buf, 8, &got));
    try std.testing.expectEqual(@as(u16, 7), got);
    try std.testing.expectEqual(pprn.pipe_bulk_out, fake.last_pipe);
    fake.fail_name = "out";
    fake.fail_code = 0x203;
    try std.testing.expectEqual(@as(u16, 0x203), s.recv(&fake, &buf, 8, &got));
    try std.testing.expectEqual(@as(u16, 0), got);
}

test "send bounds the length by the bulk packet size" {
    var fake: Fake = .{};
    var closed: pprn.State = .{};
    try std.testing.expectEqual(codes.invalid_state, closed.send(&fake, null, 1));
    const s = ready(&fake, pprn.speed_fs);
    const data = [_]u8{0} ** 65;
    try std.testing.expectEqual(codes.null_ptr, s.send(&fake, null, 1));
    try std.testing.expectEqual(codes.invalid_arg, s.send(&fake, null, 0));
    try std.testing.expectEqual(codes.invalid_arg, s.send(&fake, &data, 65));
    try std.testing.expectEqual(codes.ok, s.send(&fake, &data, 64));
    try std.testing.expectEqual(pprn.pipe_bulk_in, fake.last_pipe);
}

test "port status round-trips and needs an open class" {
    var fake: Fake = .{};
    var closed: pprn.State = .{};
    var out: u8 = 0;
    try std.testing.expectEqual(codes.invalid_state, closed.setPortStatus(0x20));
    try std.testing.expectEqual(codes.null_ptr, closed.getPortStatus(&fake, null));
    try std.testing.expectEqualStrings("get_port_status: out_status", std.mem.span(fake.logged[0]));
    try std.testing.expectEqual(codes.invalid_state, closed.getPortStatus(&fake, &out));
    var s = ready(&fake, pprn.speed_fs);
    try std.testing.expectEqual(codes.ok, s.setPortStatus(0x20));
    try std.testing.expectEqual(codes.ok, s.getPortStatus(&fake, &out));
    try std.testing.expectEqual(@as(u8, 0x20), out);
}

test "handle_setup rejects other envelopes and unknown requests" {
    var fake: Fake = .{};
    var closed: pprn.State = .{};
    var setup = pprn.Setup{ .bm_request_type = 0xC0, .b_request = 0, .w_value = 0, .w_index = 0, .w_length = 0 };
    try std.testing.expectEqual(codes.null_ptr, closed.handleSetup(&fake, null));
    try std.testing.expectEqual(codes.invalid_state, closed.handleSetup(&fake, &setup));
    const s = ready(&fake, pprn.speed_fs);
    try std.testing.expectEqual(codes.not_supported, s.handleSetup(&fake, &setup));
    setup.bm_request_type = pprn.bm_class_iface_in;
    setup.b_request = 3;
    try std.testing.expectEqual(codes.not_supported, s.handleSetup(&fake, &setup));
    try std.testing.expectEqual(@as(?bool, null), fake.accept);
}

test "handle_setup accepts without a handler and follows the handler's verdict" {
    var fake: Fake = .{};
    var s = ready(&fake, pprn.speed_fs);
    var setup = pprn.Setup{ .bm_request_type = pprn.bm_class_iface_out, .b_request = pprn.req_soft_reset, .w_value = 0, .w_index = 0, .w_length = 0 };
    try std.testing.expectEqual(codes.ok, s.handleSetup(&fake, &setup));
    try std.testing.expectEqual(@as(?bool, true), fake.accept);
    var hits: u8 = 0;
    try std.testing.expectEqual(codes.ok, s.attachSetupHandler(failHandler, &hits));
    setup.b_request = pprn.req_get_port_status;
    try std.testing.expectEqual(codes.ok, s.handleSetup(&fake, &setup));
    try std.testing.expectEqual(@as(?bool, false), fake.accept);
    try std.testing.expectEqual(@as(u8, 1), hits);
    try std.testing.expectEqual(codes.ok, s.attachSetupHandler(okHandler, null));
    setup.b_request = pprn.req_get_device_id;
    try std.testing.expectEqual(codes.ok, s.handleSetup(&fake, &setup));
    try std.testing.expectEqual(@as(?bool, true), fake.accept);
}

test "attach_setup_handler needs an open class" {
    var closed: pprn.State = .{};
    try std.testing.expectEqual(codes.invalid_state, closed.attachSetupHandler(okHandler, null));
}
