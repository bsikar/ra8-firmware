//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const cdc = @import("usb_cdc");

const Fake = struct {
    init_ret: u16 = 0,
    cfg_fail_at: ?u8 = null,
    cfg_calls: u8 = 0,
    mps: [3]u16 = .{ 0, 0, 0 },
    deinits: u8 = 0,
    detached: bool = false,
    arm_ret: u16 = 0,
    reads: u16 = 0,
    read_ret: u16 = cdc.no_data,
    payload: [8]u8 = .{ 0, 0, 0, 0, 0, 0, 0, 0 },
    payload_len: u16 = 0,
    acks: u8 = 0,
    queued_pipe: u8 = 0,
    errs: u8 = 0,
    infos: u8 = 0,

    pub fn deviceInit(f: *Fake, _: u8) u16 {
        return f.init_ret;
    }
    pub fn deviceDeinit(f: *Fake, _: u8) u16 {
        f.deinits += 1;
        return 0x203;
    }
    pub fn attach(f: *Fake, _: u8, attached: bool) u16 {
        f.detached = !attached;
        return 0;
    }
    pub fn configureEndpoint(f: *Fake, _: u8, _: u8, _: u8, _: u8, _: u8, mp: u16) u16 {
        defer f.cfg_calls += 1;
        f.mps[f.cfg_calls] = mp;
        return if (f.cfg_fail_at == f.cfg_calls) 0x203 else 0;
    }
    pub fn queueIn(f: *Fake, _: u8, pipe: u8, _: ?[*]const u8, _: u16) u16 {
        f.queued_pipe = pipe;
        return 0;
    }
    pub fn queueOut(f: *Fake, _: u8, pipe: u8, _: [*]u8, _: *u16, rearm: bool) u16 {
        f.queued_pipe = pipe;
        return if (rearm) 0 else 0x103;
    }
    pub fn dcpOutArm(f: *Fake, _: u8) u16 {
        return f.arm_ret;
    }
    pub fn dcpOutRead(f: *Fake, _: u8, buf: [*]u8, cap: u16, out: *u16) u16 {
        f.reads += 1;
        if (f.read_ret != 0) return f.read_ret;
        @memcpy(buf[0..f.payload_len], f.payload[0..f.payload_len]);
        out.* = f.payload_len;
        std.debug.assert(cap == cdc.line_coding_len + 1);
        return 0;
    }
    pub fn controlResponse(f: *Fake, _: u8, accept: bool) u16 {
        if (accept) f.acks += 1;
        return 0;
    }
    pub fn info(f: *Fake, _: [*:0]const u8) void {
        f.infos += 1;
    }
    pub fn err(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
    pub fn errVal(f: *Fake, _: [*:0]const u8, _: u32) void {
        f.errs += 1;
    }
};

fn setup(req: u8, value: u16) cdc.Setup {
    return .{ .bm_request_type = cdc.bm_class_iface, .b_request = req, .w_value = value, .w_index = 0, .w_length = 7 };
}

fn ready(f: *Fake) cdc.State {
    var s: cdc.State = .{};
    std.debug.assert(cdc.init(&s, f, cdc.speed_hs) == cdc.ok);
    return s;
}

test "init rejects a bad speed and maps a device-init failure" {
    var f: Fake = .{};
    var s: cdc.State = .{};
    try std.testing.expectEqual(cdc.invalid_arg, cdc.init(&s, &f, 2));
    f.init_ret = 0x203;
    try std.testing.expectEqual(cdc.hw_init_failed, cdc.init(&s, &f, cdc.speed_fs));
    try std.testing.expectEqual(@as(u8, 1), f.errs);
    try std.testing.expect(!s.initialized);
}

test "init sizes bulk pipes by speed and resets the line coding" {
    var f: Fake = .{};
    var s: cdc.State = .{ .coding = .{ .dte_rate = 1, .char_format = 2, .parity_type = 3, .data_bits = 4 } };
    try std.testing.expectEqual(cdc.ok, cdc.init(&s, &f, cdc.speed_hs));
    try std.testing.expectEqualSlices(u16, &.{ 512, 512, 8 }, &f.mps);
    try std.testing.expectEqual(cdc.default_coding, s.coding);
    try std.testing.expectEqual(@as(u8, 1), f.infos);
    f = .{};
    _ = cdc.init(&s, &f, cdc.speed_fs);
    try std.testing.expectEqualSlices(u16, &.{ 64, 64, 8 }, &f.mps);
}

test "a pipe failure deinits the device and logs twice for bulk pipes" {
    var f: Fake = .{ .cfg_fail_at = 1 };
    var s: cdc.State = .{};
    try std.testing.expectEqual(@as(u16, 0x203), cdc.init(&s, &f, cdc.speed_fs));
    try std.testing.expectEqual(@as(u8, 1), f.deinits);
    try std.testing.expectEqual(@as(u8, 2), f.errs);
    f = .{ .cfg_fail_at = 2 };
    try std.testing.expectEqual(@as(u16, 0x203), cdc.init(&s, &f, cdc.speed_fs));
    try std.testing.expectEqual(@as(u8, 0), f.errs);
    try std.testing.expect(!s.initialized);
}

test "calls before init report invalid state" {
    var f: Fake = .{};
    var s: cdc.State = .{};
    var buf: [1]u8 = undefined;
    var n: u16 = 1;
    const st = setup(cdc.req_get_line_coding, 0);
    try std.testing.expectEqual(cdc.invalid_state, cdc.deinit(&s, &f));
    try std.testing.expectEqual(cdc.invalid_state, cdc.attach(&s, &f, true));
    try std.testing.expectEqual(cdc.invalid_state, cdc.send(&s, &f, null, 0));
    try std.testing.expectEqual(cdc.invalid_state, cdc.recv(&s, &f, &buf, &n));
    try std.testing.expectEqual(cdc.invalid_state, cdc.handleSetup(&s, &f, &st));
}

test "deinit detaches, clears line state and returns the deinit code" {
    var f: Fake = .{};
    var s = ready(&f);
    s.dtr = true;
    try std.testing.expectEqual(@as(u16, 0x203), cdc.deinit(&s, &f));
    try std.testing.expect(f.detached and !s.initialized and !s.dtr);
}

test "send and recv validate then use the bulk pipes" {
    var f: Fake = .{};
    var s = ready(&f);
    var buf: [4]u8 = undefined;
    var n: u16 = 0;
    try std.testing.expectEqual(cdc.invalid_arg, cdc.send(&s, &f, null, 3));
    try std.testing.expectEqual(cdc.ok, cdc.send(&s, &f, null, 0));
    try std.testing.expectEqual(cdc.pipe_bulk_in, f.queued_pipe);
    try std.testing.expectEqual(cdc.invalid_arg, cdc.recv(&s, &f, &buf, &n));
    n = 4;
    try std.testing.expectEqual(cdc.ok, cdc.recv(&s, &f, &buf, &n));
    try std.testing.expectEqual(cdc.pipe_bulk_out, f.queued_pipe);
}

test "apply line coding decodes little-endian and ignores short payloads" {
    var s: cdc.State = .{};
    const p = [_]u8{ 0x00, 0xC2, 0x01, 0x00, 2, 1, 7 };
    cdc.applyLineCoding(&s, &p, 6);
    try std.testing.expectEqual(cdc.default_coding, s.coding);
    cdc.applyLineCoding(&s, null, 7);
    try std.testing.expectEqual(cdc.default_coding, s.coding);
    cdc.applyLineCoding(&s, &p, 7);
    try std.testing.expectEqual(cdc.LineCoding{ .dte_rate = 115200, .char_format = 2, .parity_type = 1, .data_bits = 7 }, s.coding);
}

test "set line coding pulls the data stage and acks even when it fails" {
    var f: Fake = .{};
    var s = ready(&f);
    f.read_ret = 0;
    f.payload = .{ 0x80, 0x25, 0, 0, 0, 0, 8, 0 };
    f.payload_len = 7;
    const st = setup(cdc.req_set_line_coding, 0);
    try std.testing.expectEqual(cdc.ok, cdc.handleSetup(&s, &f, &st));
    try std.testing.expectEqual(@as(u32, 0x2580), s.coding.dte_rate);
    f.read_ret = cdc.no_data;
    f.reads = 0;
    try std.testing.expectEqual(cdc.ok, cdc.handleSetup(&s, &f, &st));
    try std.testing.expectEqual(cdc.data_stage_polls, f.reads);
    f.arm_ret = 0x203;
    f.reads = 0;
    try std.testing.expectEqual(cdc.ok, cdc.handleSetup(&s, &f, &st));
    try std.testing.expectEqual(@as(u16, 0), f.reads);
    try std.testing.expectEqual(@as(u8, 3), f.acks);
}

test "control line state sets DTR and RTS from wValue" {
    var f: Fake = .{};
    var s = ready(&f);
    var st = setup(cdc.req_set_control_line_state, 0x3);
    try std.testing.expectEqual(cdc.ok, cdc.handleSetup(&s, &f, &st));
    try std.testing.expect(s.dtr and s.rts);
    st.w_value = cdc.line_state_rts;
    _ = cdc.handleSetup(&s, &f, &st);
    try std.testing.expect(!s.dtr and s.rts);
}

test "setup rejects other request types and unknown requests" {
    var f: Fake = .{};
    var s = ready(&f);
    var st = setup(cdc.req_get_line_coding, 0);
    st.bm_request_type = 0x80;
    try std.testing.expectEqual(cdc.not_supported, cdc.handleSetup(&s, &f, &st));
    st.bm_request_type = cdc.bm_class_in;
    try std.testing.expectEqual(cdc.ok, cdc.handleSetup(&s, &f, &st));
    st.b_request = 0x23;
    try std.testing.expectEqual(cdc.not_supported, cdc.handleSetup(&s, &f, &st));
}
