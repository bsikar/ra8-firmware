//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/usb_phid.zig.

const std = @import("std");
const phid = @import("usb_phid");
const codes = phid.codes;

/// Records each primitive call; `fail_name` sets the code a named call returns.
const Fake = struct {
    calls: [16][]const u8 = undefined,
    count: usize = 0,
    fail_name: []const u8 = "",
    fail_code: u16 = 0,
    pipes: [2]u8 = .{ 0, 0 },
    kinds: [2]u8 = .{ 0, 0 },
    eps_seen: usize = 0,
    last_mp: u16 = 0,
    in_lens: [8]u16 = undefined,
    in_first: [8]u8 = undefined,
    ins: usize = 0,
    accept: ?bool = null,
    out_len: u16 = 5,
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
    pub fn deviceAttach(self: *Fake, _: u8, attached: bool) u16 {
        std.debug.assert(!attached);
        return self.hit("attach");
    }
    pub fn configureEndpoint(self: *Fake, _: u8, pipe: u8, _: u8, _: u8, kind: u8, mp: u16) u16 {
        self.pipes[self.eps_seen] = pipe;
        self.kinds[self.eps_seen] = kind;
        self.eps_seen += 1;
        self.last_mp = mp;
        return self.hit("ep");
    }
    pub fn queueIn(self: *Fake, _: u8, pipe: u8, data: [*]const u8, len: u16) u16 {
        std.debug.assert(pipe == phid.pipe_intr_in);
        self.in_lens[self.ins] = len;
        self.in_first[self.ins] = if (len > 0) data[0] else 0;
        self.ins += 1;
        return self.hit("in");
    }
    pub fn queueOut(self: *Fake, _: u8, pipe: u8, _: [*]u8, inout: *u16, rearm: bool) u16 {
        std.debug.assert(pipe == phid.pipe_intr_out and rearm);
        inout.* = self.out_len;
        return self.hit("out");
    }
    pub fn controlResponse(self: *Fake, _: u8, accept: bool) u16 {
        self.accept = accept;
        return self.hit("ctrl");
    }
    pub fn logError(self: *Fake, _: [*:0]const u8) void {
        self.logs += 1;
    }
    pub fn logErrorVal(self: *Fake, _: [*:0]const u8, _: u16) void {
        self.logs += 1;
    }
    pub fn logInfoVal(_: *Fake, _: [*:0]const u8, _: u8) void {}
};

fn ready(f: *Fake, speed: u8) !phid.State {
    var s: phid.State = .{};
    try std.testing.expectEqual(codes.ok, s.init(f, speed));
    return s;
}

test "init checks speed, maps a device failure and opens two interrupt pipes" {
    var f: Fake = .{};
    var s: phid.State = .{};
    try std.testing.expectEqual(codes.invalid_arg, s.init(&f, 2));
    f.fail_name = "init";
    f.fail_code = 0x203;
    try std.testing.expectEqual(codes.hw_init_failed, s.init(&f, phid.speed_fs));
    try std.testing.expect(!s.initialized);
    f = .{};
    s = try ready(&f, phid.speed_hs);
    try std.testing.expectEqual([2]u8{ phid.pipe_intr_in, phid.pipe_intr_out }, f.pipes);
    try std.testing.expectEqual([2]u8{ phid.type_intr, phid.type_intr }, f.kinds);
    try std.testing.expectEqual(phid.max_packet_hs, f.last_mp);
    try std.testing.expectEqual(phid.proto_report, s.protocol);
    f = .{};
    s = try ready(&f, phid.speed_fs);
    try std.testing.expectEqual(phid.max_packet_default, s.intr_max_packet);
}

test "calls before init report invalid_state" {
    var f: Fake = .{};
    var s: phid.State = .{};
    var b: [4]u8 = undefined;
    var n: u16 = 0;
    var v: u8 = 0;
    const setup = phid.Setup{ .bm_request_type = phid.bm_class_iface_out, .b_request = phid.req_set_idle, .w_value = 0, .w_index = 0, .w_length = 0 };
    try std.testing.expectEqual(codes.invalid_state, s.close(&f));
    try std.testing.expectEqual(codes.invalid_state, s.setDescriptors(&f, &b, 1, &b, 1));
    try std.testing.expectEqual(codes.invalid_state, s.sendReport(&f, 1, &b, 1));
    try std.testing.expectEqual(codes.invalid_state, s.recvReport(&f, &b, 4, &n));
    try std.testing.expectEqual(codes.invalid_state, s.attachSetupHandler(null, null));
    try std.testing.expectEqual(codes.invalid_state, s.handleSetup(&f, &setup));
    try std.testing.expectEqual(codes.invalid_state, s.getIdle(&f, &v));
    try std.testing.expectEqual(codes.invalid_state, s.getProtocol(&f, &v));
    try std.testing.expectEqual(@as(usize, 0), f.count);
}

test "set_descriptors needs both descriptors with lengths" {
    var f: Fake = .{};
    var s = try ready(&f, phid.speed_fs);
    const d = [_]u8{ 1, 2, 3 };
    try std.testing.expectEqual(codes.null_ptr, s.setDescriptors(&f, null, 3, &d, 3));
    try std.testing.expectEqual(codes.null_ptr, s.setDescriptors(&f, &d, 3, null, 3));
    try std.testing.expectEqual(codes.invalid_arg, s.setDescriptors(&f, &d, 0, &d, 3));
    try std.testing.expectEqual(codes.invalid_arg, s.setDescriptors(&f, &d, 3, &d, 0));
    try std.testing.expectEqual(codes.ok, s.setDescriptors(&f, &d, 3, &d, 2));
    try std.testing.expectEqual(@as(u16, 2), s.hid_desc_len);
    try std.testing.expectEqual(@as(usize, 2), f.logs);
}

test "send_report prepends the report ID and checks the framed length" {
    var f: Fake = .{};
    const s = try ready(&f, phid.speed_fs);
    const p = [_]u8{ 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    try std.testing.expectEqual(codes.null_ptr, s.sendReport(&f, 1, null, 1));
    try std.testing.expectEqual(codes.invalid_arg, s.sendReport(&f, 0, null, 0));
    try std.testing.expectEqual(codes.invalid_arg, s.sendReport(&f, 2, &p, 8));
    try std.testing.expectEqual(codes.ok, s.sendReport(&f, 0, &p, 8));
    try std.testing.expectEqual(codes.ok, s.sendReport(&f, 3, &p, 7));
    try std.testing.expectEqual(@as(usize, 3), f.ins);
    try std.testing.expectEqual([3]u16{ 8, 1, 7 }, f.in_lens[0..3].*);
    try std.testing.expectEqual(@as(u8, 3), f.in_first[1]);
    try std.testing.expectEqual(codes.ok, s.sendReport(&f, 4, null, 0));
    f.fail_name = "in";
    f.fail_code = 0x204;
    const before = f.ins;
    try std.testing.expectEqual(@as(u16, 0x204), s.sendReport(&f, 5, &p, 1));
    try std.testing.expectEqual(before + 1, f.ins);
}

test "recv_report reports the received length, or zero on error" {
    var f: Fake = .{};
    const s = try ready(&f, phid.speed_fs);
    var b: [8]u8 = undefined;
    var n: u16 = 99;
    try std.testing.expectEqual(codes.null_ptr, s.recvReport(&f, null, 8, &n));
    try std.testing.expectEqual(codes.null_ptr, s.recvReport(&f, &b, 8, null));
    try std.testing.expectEqual(codes.invalid_arg, s.recvReport(&f, &b, 0, &n));
    try std.testing.expectEqual(codes.ok, s.recvReport(&f, &b, 8, &n));
    try std.testing.expectEqual(@as(u16, 5), n);
    f.fail_name = "out";
    f.fail_code = 0x203;
    try std.testing.expectEqual(@as(u16, 0x203), s.recvReport(&f, &b, 8, &n));
    try std.testing.expectEqual(@as(u16, 0), n);
}

var cb_rc: u16 = 0;
var cb_hits: u32 = 0;
fn handler(_: ?*anyopaque, _: *const phid.Setup) callconv(.c) u16 {
    cb_hits += 1;
    return cb_rc;
}

fn req(bm: u8, b: u8, value: u16) phid.Setup {
    return .{ .bm_request_type = bm, .b_request = b, .w_value = value, .w_index = 0, .w_length = 0 };
}

test "class setups update idle and protocol, then the handler picks ACK or stall" {
    var f: Fake = .{};
    var s = try ready(&f, phid.speed_fs);
    var v: u8 = 0;
    try std.testing.expectEqual(codes.not_supported, s.handleSetup(&f, &req(0x40, phid.req_set_idle, 0)));
    try std.testing.expectEqual(codes.not_supported, s.handleSetup(&f, &req(phid.bm_class_iface_out, 0x05, 0)));
    try std.testing.expectEqual(codes.ok, s.handleSetup(&f, &req(phid.bm_class_iface_out, phid.req_set_idle, 0x7D00)));
    try std.testing.expectEqual(true, f.accept.?);
    try std.testing.expectEqual(codes.ok, s.getIdle(&f, &v));
    try std.testing.expectEqual(@as(u8, 0x7D), v);
    try std.testing.expectEqual(codes.ok, s.handleSetup(&f, &req(phid.bm_class_iface_out, phid.req_set_protocol, 0)));
    try std.testing.expectEqual(codes.ok, s.getProtocol(&f, &v));
    try std.testing.expectEqual(phid.proto_boot, v);
    try std.testing.expectEqual(codes.ok, s.handleSetup(&f, &req(phid.bm_class_iface_out, phid.req_set_protocol, 7)));
    try std.testing.expectEqual(codes.ok, s.getProtocol(&f, &v));
    try std.testing.expectEqual(phid.proto_boot, v);
    try std.testing.expectEqual(codes.ok, s.attachSetupHandler(&handler, null));
    cb_rc = 0x107;
    cb_hits = 0;
    try std.testing.expectEqual(codes.ok, s.handleSetup(&f, &req(phid.bm_class_iface_in, phid.req_get_report, 0x0100)));
    try std.testing.expectEqual(false, f.accept.?);
    cb_rc = 0;
    try std.testing.expectEqual(codes.ok, s.handleSetup(&f, &req(phid.bm_class_iface_in, phid.req_get_idle, 0)));
    try std.testing.expectEqual(true, f.accept.?);
    try std.testing.expectEqual(@as(u32, 2), cb_hits);
    try std.testing.expectEqual(codes.null_ptr, s.handleSetup(&f, null));
    try std.testing.expectEqual(codes.null_ptr, s.getIdle(&f, null));
    try std.testing.expectEqual(codes.null_ptr, s.getProtocol(&f, null));
}

test "close detaches, returns the deinit result and drops the handler" {
    var f: Fake = .{};
    var s = try ready(&f, phid.speed_hs);
    try std.testing.expectEqual(codes.ok, s.attachSetupHandler(&handler, null));
    f.fail_name = "deinit";
    f.fail_code = 0x204;
    try std.testing.expectEqual(@as(u16, 0x204), s.close(&f));
    try std.testing.expect(!s.initialized);
    try std.testing.expectEqual(@as(?phid.SetupFn, null), s.setup_cb);
    try std.testing.expectEqualStrings("attach", f.calls[f.count - 2]);
}
