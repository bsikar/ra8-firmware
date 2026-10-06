//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const pa = @import("usb_paud");

const Fake = struct {
    init_ret: u16 = 0,
    deinit_ret: u16 = 0,
    out_len: u16 = 0,
    out_ret: u16 = 0,
    configured: u8 = 0,
    last_mp: u16 = 0,
    attached: ?bool = null,
    accepted: ?bool = null,
    queued: u16 = 0,
    errs: u8 = 0,
    infos: u8 = 0,

    pub fn deviceInit(f: *Fake, _: u8) u16 {
        return f.init_ret;
    }
    pub fn deviceDeinit(f: *Fake, _: u8) u16 {
        return f.deinit_ret;
    }
    pub fn deviceAttach(f: *Fake, _: u8, a: bool) u16 {
        f.attached = a;
        return 0;
    }
    pub fn configureEndpoint(f: *Fake, _: u8, _: u8, _: u8, _: u8, ty: u8, mp: u16) u16 {
        std.debug.assert(ty == pa.type_iso);
        f.configured += 1;
        f.last_mp = mp;
        return 0;
    }
    pub fn queueIn(f: *Fake, _: u8, pipe: u8, _: [*]const u8, len: u16) u16 {
        std.debug.assert(pipe == pa.pipe_iso_in);
        f.queued = len;
        return 0;
    }
    pub fn queueOut(f: *Fake, _: u8, _: u8, _: [*]u8, inout: *u16, _: bool) u16 {
        inout.* = f.out_len;
        return f.out_ret;
    }
    pub fn controlResponse(f: *Fake, _: u8, accept: bool) u16 {
        f.accepted = accept;
        return 0;
    }
    pub fn err(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
    pub fn errVal(f: *Fake, _: [*:0]const u8, _: u32) void {
        f.errs += 1;
    }
    pub fn infoVal(f: *Fake, _: [*:0]const u8, _: u32) void {
        f.infos += 1;
    }
};

fn ready(s: *pa.State, f: *Fake, speed: u8) !void {
    try std.testing.expectEqual(pa.ok, pa.init(s, f, speed));
}

test "init rejects a bad speed before touching the device" {
    var s = pa.State{};
    var f = Fake{};
    try std.testing.expectEqual(pa.invalid_arg, pa.init(&s, &f, 2));
    try std.testing.expectEqual(@as(u8, 0), f.configured);
}

test "device init failure logs and maps to hw_init_failed" {
    var s = pa.State{};
    var f = Fake{ .init_ret = 0x109 };
    try std.testing.expectEqual(pa.hw_init_failed, pa.init(&s, &f, pa.speed_fs));
    try std.testing.expectEqual(@as(u8, 1), f.errs);
    try std.testing.expect(!s.initialized);
}

test "init sets the default format and both iso pipes" {
    var s = pa.State{};
    var f = Fake{};
    try ready(&s, &f, pa.speed_hs);
    try std.testing.expectEqual(@as(u8, 2), f.configured);
    try std.testing.expectEqual(@as(u16, 1024), f.last_mp);
    try std.testing.expectEqual(@as(u32, 48000), s.format.sample_rate_hz);
    try std.testing.expectEqual(@as(u8, 1), f.infos);
}

test "calls before init report invalid_state" {
    var s = pa.State{};
    var f = Fake{};
    const b = [_]u8{1};
    try std.testing.expectEqual(pa.invalid_state, pa.close(&s, &f));
    try std.testing.expectEqual(pa.invalid_state, pa.sendFrame(&s, &f, &b, 1));
    try std.testing.expectEqual(pa.invalid_state, pa.setFormat(&s, pa.default_format));
}

test "send_frame checks null, then size against the FS packet" {
    var s = pa.State{};
    var f = Fake{};
    try ready(&s, &f, pa.speed_fs);
    var b: [200]u8 = @splat(0);
    try std.testing.expectEqual(pa.null_ptr, pa.sendFrame(&s, &f, null, 4));
    try std.testing.expectEqual(pa.invalid_arg, pa.sendFrame(&s, &f, null, 0));
    try std.testing.expectEqual(pa.invalid_arg, pa.sendFrame(&s, &f, &b, 193));
    try std.testing.expectEqual(pa.ok, pa.sendFrame(&s, &f, &b, 192));
    try std.testing.expectEqual(@as(u16, 192), f.queued);
}

test "recv_frame checks pointers first and zeroes got_len on error" {
    var s = pa.State{};
    var f = Fake{ .out_len = 7 };
    var b: [8]u8 = @splat(0);
    var got: u16 = 99;
    try std.testing.expectEqual(pa.null_ptr, pa.recvFrame(&s, &f, null, 8, &got));
    try std.testing.expectEqual(pa.invalid_state, pa.recvFrame(&s, &f, &b, 8, &got));
    try ready(&s, &f, pa.speed_fs);
    try std.testing.expectEqual(pa.ok, pa.recvFrame(&s, &f, &b, 8, &got));
    try std.testing.expectEqual(@as(u16, 7), got);
    f.out_ret = 0x10A;
    try std.testing.expectEqual(@as(u16, 0x10A), pa.recvFrame(&s, &f, &b, 8, &got));
    try std.testing.expectEqual(@as(u16, 0), got);
}

test "set_format enforces rate, channel and sample-size bounds" {
    var s = pa.State{};
    var f = Fake{};
    try ready(&s, &f, pa.speed_fs);
    try std.testing.expectEqual(pa.invalid_arg, pa.setFormat(&s, .{ .sample_rate_hz = 0, .channels = 1, .bytes_per_sample = 2 }));
    try std.testing.expectEqual(pa.invalid_arg, pa.setFormat(&s, .{ .sample_rate_hz = 8000, .channels = 3, .bytes_per_sample = 2 }));
    try std.testing.expectEqual(pa.invalid_arg, pa.setFormat(&s, .{ .sample_rate_hz = 8000, .channels = 1, .bytes_per_sample = 5 }));
    try std.testing.expectEqual(pa.ok, pa.setFormat(&s, .{ .sample_rate_hz = 8000, .channels = 1, .bytes_per_sample = 4 }));
    try std.testing.expectEqual(@as(u32, 8000), s.format.sample_rate_hz);
}

fn failCb(_: ?*anyopaque, _: *const pa.Setup) callconv(.c) u16 {
    return 0x104;
}

test "handle_setup filters envelopes and requests, NAKs a failing handler" {
    var s = pa.State{};
    var f = Fake{};
    try ready(&s, &f, pa.speed_fs);
    var p = pa.Setup{ .bm_request_type = 0x80, .b_request = 0x01, .w_value = 0, .w_index = 0, .w_length = 0 };
    try std.testing.expectEqual(pa.not_supported, pa.handleSetup(&s, &f, &p));
    p.bm_request_type = 0x21;
    p.b_request = 0x05;
    try std.testing.expectEqual(pa.not_supported, pa.handleSetup(&s, &f, &p));
    p.b_request = 0xFF;
    try std.testing.expectEqual(pa.ok, pa.handleSetup(&s, &f, &p));
    try std.testing.expectEqual(@as(?bool, true), f.accepted);
    s.setup_cb = failCb;
    try std.testing.expectEqual(pa.ok, pa.handleSetup(&s, &f, &p));
    try std.testing.expectEqual(@as(?bool, false), f.accepted);
}

test "close detaches, returns the deinit code and clears the handler" {
    var s = pa.State{};
    var f = Fake{ .deinit_ret = 0x203 };
    try ready(&s, &f, pa.speed_fs);
    s.setup_cb = failCb;
    try std.testing.expectEqual(@as(u16, 0x203), pa.close(&s, &f));
    try std.testing.expectEqual(@as(?bool, false), f.attached);
    try std.testing.expect(!s.initialized and s.setup_cb == null);
}

test "set_descriptors needs a pointer and a length" {
    var s = pa.State{};
    var f = Fake{};
    try ready(&s, &f, pa.speed_fs);
    const d = [_]u8{ 9, 2 };
    try std.testing.expectEqual(pa.null_ptr, pa.setDescriptors(&s, &f, null, 2));
    try std.testing.expectEqual(pa.invalid_arg, pa.setDescriptors(&s, &f, &d, 0));
    try std.testing.expectEqual(pa.ok, pa.setDescriptors(&s, &f, &d, 2));
    try std.testing.expectEqual(@as(u16, 2), s.desc_len);
}
