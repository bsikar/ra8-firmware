// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const backend = @import("backend_fake");

const cfg = struct {
    const Session = backend.Session;
};
const Err = backend.Status;

/// A scripted transport: records what the backend handed it and replays the
/// status the case under test wants back.
const Wire = struct {
    var sent: [8]u8 = [_]u8{0} ** 8;
    var sent_len: usize = 0;
    var send_status: u16 = Err.ok;
    var recv_status: u16 = Err.ok;
    var recv_byte: u8 = 0;
    var recv_calls: usize = 0;

    fn reset() void {
        sent_len = 0;
        send_status = Err.ok;
        recv_status = Err.ok;
        recv_byte = 0;
        recv_calls = 0;
    }

    fn send(_: ?*anyopaque, buf: ?[*]const u8, len: usize, out_sent: ?*usize) callconv(.c) u16 {
        if (send_status != Err.ok) return send_status;
        @memcpy(sent[0..len], buf.?[0..len]);
        sent_len = len;
        out_sent.?.* = len;
        return Err.ok;
    }

    fn recv(_: ?*anyopaque, buf: ?[*]u8, len: usize, out_received: ?*usize) callconv(.c) u16 {
        recv_calls += 1;
        if (recv_status != Err.ok) return recv_status;
        buf.?[0] = recv_byte;
        out_received.?.* = @min(len, 1);
        return Err.ok;
    }

    fn session() cfg.Session {
        return .{ .transport = .{ .send = send, .recv = recv, .ctx = null } };
    }
};

test "a handshake is one record byte out and one byte back" {
    Wire.reset();
    const config = Wire.session();
    var state: backend.SlotState = .{};
    try std.testing.expectEqual(Err.ok, backend.sessionSetup(&state, &config));

    try std.testing.expectEqual(Err.ok, backend.handshake(&state, &config));
    try std.testing.expectEqual(@as(usize, 1), Wire.sent_len);
    try std.testing.expectEqual(backend.content_handshake, Wire.sent[0]);
    try std.testing.expectEqual(@as(usize, 1), Wire.recv_calls);
    try std.testing.expect(state.handshake_done);
}

test "a blocked send stays would_block and never reaches the read half" {
    Wire.reset();
    Wire.send_status = Err.would_block;
    const config = Wire.session();
    var state: backend.SlotState = .{};

    try std.testing.expectEqual(Err.would_block, backend.handshake(&state, &config));
    try std.testing.expectEqual(@as(usize, 0), Wire.recv_calls);
    try std.testing.expect(!state.handshake_done);
}

test "any other send failure becomes a comm error" {
    Wire.reset();
    Wire.send_status = Err.hw_error;
    const config = Wire.session();
    var state: backend.SlotState = .{};
    try std.testing.expectEqual(Err.comm_error, backend.handshake(&state, &config));
}

test "a blocked read is would_block and a failed read is a comm error" {
    const config = Wire.session();
    var state: backend.SlotState = .{};

    Wire.reset();
    Wire.recv_status = Err.would_block;
    try std.testing.expectEqual(Err.would_block, backend.handshake(&state, &config));
    try std.testing.expect(!state.handshake_done);

    Wire.reset();
    Wire.recv_status = Err.comm_error;
    try std.testing.expectEqual(Err.comm_error, backend.handshake(&state, &config));
}

test "payloads pass straight through to the transport" {
    Wire.reset();
    const config = Wire.session();
    var state: backend.SlotState = .{};

    var moved: usize = 0;
    try std.testing.expectEqual(Err.ok, backend.send(&state, &config, "hey", &moved));
    try std.testing.expectEqual(@as(usize, 3), moved);
    try std.testing.expectEqualStrings("hey", Wire.sent[0..3]);

    Wire.recv_byte = 'z';
    var inbox: [4]u8 = [_]u8{0} ** 4;
    try std.testing.expectEqual(Err.ok, backend.recv(&state, &config, &inbox, &moved));
    try std.testing.expectEqual(@as(usize, 1), moved);
    try std.testing.expectEqual(@as(u8, 'z'), inbox[0]);
}

test "the reported suite is the deterministic loopback name with a zero id" {
    var state: backend.SlotState = .{};
    var id: u16 = 0xFFFF;
    var name: []const u8 = &.{};
    backend.cipherSuite(&state, &id, &name);
    try std.testing.expectEqual(@as(u16, 0), id);
    try std.testing.expectEqualStrings("off-target-loopback", name);
    try std.testing.expectEqual(@as(u32, 0), backend.verifyResult(&state));
}
