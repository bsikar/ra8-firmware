// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const facade = @import("facade");

const cfg = struct {
    const Session = facade.Session;
};
const Err = facade.Status;

fn okSend(_: ?*anyopaque, _: ?[*]const u8, len: usize, out_sent: ?*usize) callconv(.c) u16 {
    out_sent.?.* = len;
    return Err.ok;
}

fn okRecv(_: ?*anyopaque, buf: ?[*]u8, len: usize, out_received: ?*usize) callconv(.c) u16 {
    if (len > 0) buf.?[0] = 0x16;
    out_received.?.* = @min(len, 1);
    return Err.ok;
}

fn config() cfg.Session {
    return .{ .transport = .{ .send = okSend, .recv = okRecv, .ctx = null } };
}

fn cold() void {
    facade.resetForTest();
}

fn opened(want: *const cfg.Session) *facade.Slot {
    var handle: ?*facade.Slot = null;
    std.debug.assert(facade.sessionOpen(want, &handle) == Err.ok);
    return handle.?;
}

test "init is one-shot and deinit is its mirror" {
    cold();
    try std.testing.expectEqual(Err.not_initialized, facade.globalDeinit());
    try std.testing.expectEqual(Err.ok, facade.globalInit());
    try std.testing.expectEqual(Err.exists, facade.globalInit());
    try std.testing.expectEqual(Err.ok, facade.globalDeinit());
    try std.testing.expectEqual(Err.not_initialized, facade.globalDeinit());
}

test "every entry point refuses to run before init" {
    cold();
    var handle: ?*facade.Slot = null;
    var moved: usize = 0;
    var id: u16 = 0;
    var flags: u32 = 0;
    var name: [8]u8 = [_]u8{0} ** 8;

    const want = config();
    try std.testing.expectEqual(Err.not_initialized, facade.sessionOpen(&want, &handle));
    try std.testing.expectEqual(Err.not_initialized, facade.sessionClose(null));
    try std.testing.expectEqual(Err.not_initialized, facade.handshake(null));
    try std.testing.expectEqual(Err.not_initialized, facade.send(null, "x", &moved));
    try std.testing.expectEqual(Err.not_initialized, facade.recv(null, &name, &moved));
    try std.testing.expectEqual(Err.not_initialized, facade.cipherSuite(null, &id, &name));
    try std.testing.expectEqual(Err.not_initialized, facade.verifyResult(null, &flags));
}

test "open rejects a missing config and a half-wired transport" {
    cold();
    _ = facade.globalInit();
    var handle: ?*facade.Slot = null;

    try std.testing.expectEqual(Err.invalid_arg, facade.sessionOpen(null, &handle));

    var half = config();
    half.transport.recv = null;
    try std.testing.expectEqual(Err.invalid_arg, facade.sessionOpen(&half, &handle));

    half = config();
    half.transport.send = null;
    try std.testing.expectEqual(Err.invalid_arg, facade.sessionOpen(&half, &handle));
}

test "the pool runs out after four sessions and recovers on close" {
    cold();
    _ = facade.globalInit();
    const want = config();

    var held: [4]*facade.Slot = undefined;
    for (&held) |*slot| slot.* = opened(&want);

    var overflow: ?*facade.Slot = null;
    try std.testing.expectEqual(Err.no_mem, facade.sessionOpen(&want, &overflow));
    try std.testing.expect(overflow == null);

    try std.testing.expectEqual(Err.ok, facade.sessionClose(held[1]));
    try std.testing.expectEqual(Err.ok, facade.sessionOpen(&want, &overflow));
}

test "a closed handle stops being valid" {
    cold();
    _ = facade.globalInit();
    const want = config();
    const slot = opened(&want);

    try std.testing.expectEqual(Err.ok, facade.sessionClose(slot));
    try std.testing.expectEqual(Err.invalid_arg, facade.sessionClose(slot));
    try std.testing.expectEqual(Err.invalid_arg, facade.handshake(slot));
}

test "deinit drops every live session" {
    cold();
    _ = facade.globalInit();
    const want = config();
    const slot = opened(&want);

    try std.testing.expectEqual(Err.ok, facade.globalDeinit());
    try std.testing.expectEqual(Err.ok, facade.globalInit());
    try std.testing.expectEqual(Err.invalid_arg, facade.handshake(slot));
}

test "a null buffer with a length is rejected, and a zero length is a no-op" {
    cold();
    _ = facade.globalInit();
    const want = config();
    const slot = opened(&want);
    var moved: usize = 0;

    try std.testing.expectEqual(Err.invalid_arg, facade.send(slot, null, &moved));
    try std.testing.expectEqual(Err.invalid_arg, facade.recv(slot, null, &moved));

    const empty_in: []const u8 = &.{};
    var empty_out: [0]u8 = undefined;
    try std.testing.expectEqual(Err.ok, facade.send(slot, empty_in, &moved));
    try std.testing.expectEqual(Err.ok, facade.recv(slot, &empty_out, &moved));
    try std.testing.expectEqual(@as(usize, 0), moved);
}

test "a handshake drives the transport and traffic flows after it" {
    cold();
    _ = facade.globalInit();
    const want = config();
    const slot = opened(&want);

    try std.testing.expectEqual(Err.ok, facade.handshake(slot));

    var moved: usize = 0;
    try std.testing.expectEqual(Err.ok, facade.send(slot, "hello", &moved));
    try std.testing.expectEqual(@as(usize, 5), moved);

    var inbox: [4]u8 = [_]u8{0} ** 4;
    try std.testing.expectEqual(Err.ok, facade.recv(slot, &inbox, &moved));
    try std.testing.expectEqual(@as(usize, 1), moved);
}

test "the cipher suite lands in the caller's buffer and truncates to fit" {
    cold();
    _ = facade.globalInit();
    const want = config();
    const slot = opened(&want);

    var id: u16 = 0xFFFF;
    var roomy: [32]u8 = [_]u8{0xAA} ** 32;
    try std.testing.expectEqual(Err.ok, facade.cipherSuite(slot, &id, &roomy));
    try std.testing.expectEqual(@as(u16, 0), id);
    try std.testing.expectEqualStrings("off-target-loopback", roomy[0..19]);
    try std.testing.expectEqual(@as(u8, 0), roomy[19]);

    var tight: [4]u8 = [_]u8{0xAA} ** 4;
    try std.testing.expectEqual(Err.ok, facade.cipherSuite(slot, &id, &tight));
    try std.testing.expectEqualStrings("off", tight[0..3]);
    try std.testing.expectEqual(@as(u8, 0), tight[3]);
}

test "the verify result is clean on the loopback path" {
    cold();
    _ = facade.globalInit();
    const want = config();
    const slot = opened(&want);

    var flags: u32 = 0xDEAD;
    try std.testing.expectEqual(Err.ok, facade.verifyResult(slot, &flags));
    try std.testing.expectEqual(@as(u32, 0), flags);
}

test "a stack pointer is never mistaken for a session" {
    cold();
    _ = facade.globalInit();
    var forged: facade.Slot = std.mem.zeroes(facade.Slot);
    try std.testing.expectEqual(Err.invalid_arg, facade.handshake(&forged));
    try std.testing.expectEqual(Err.invalid_arg, facade.sessionClose(&forged));
}
