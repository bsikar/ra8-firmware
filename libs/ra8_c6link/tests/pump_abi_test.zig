//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_c6link_pump` through its C ABI, on a real `ra8_c6link_t` laid out by
//! the public header. The C dispatcher is stood in for by an export below.

const std = @import("std");
const pump_abi = @import("pump_abi");

const c = pump_abi.c;
const frame_bytes: u16 = c.k_ra8_c6link_frame_bytes;

/// What the stand-in transport and dispatcher saw.
const Bench = struct {
    armed: bool = true,
    fail: bool = false,
    reply: [frame_bytes]u8 = [_]u8{0} ** frame_bytes,
    dispatched: usize = 0,
    stats_seen: bool = false,
};

var bench: Bench = .{};

fn transfer(ctx: ?*anyopaque, tx: [*c]const u8, rx: [*c]u8, len: u16) callconv(.c) c.ra8_err_t {
    _ = ctx;
    _ = tx;
    if (bench.fail) return @intCast(c.k_ra8_err_spi_error);
    @memcpy(rx[0..len], bench.reply[0..len]);
    return @intCast(c.k_ra8_ok);
}

fn handshakeActive(ctx: ?*anyopaque) callconv(.c) bool {
    _ = ctx;
    return bench.armed;
}

fn delayMs(ctx: ?*anyopaque, ms: u16) callconv(.c) void {
    _ = ctx;
    _ = ms;
}

export fn priv_c6link_dispatch(link: *c.ra8_c6link_t, view: *const pump_abi.RxView) callconv(.c) bool {
    bench.stats_seen = link.stats != null;
    bench.dispatched += 1;
    return view.len == 8;
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.transport.transfer = transfer;
    link.transport.handshake_active = handshakeActive;
    link.transport.delay_ms = delayMs;
    return link;
}

/// A data reply: header offset 12, payload length 8, interface 1, checksum.
fn sealedReply() [frame_bytes]u8 {
    var buf = [_]u8{0} ** frame_bytes;
    buf[0] = 1;
    std.mem.writeInt(u16, buf[2..4], 8, .little);
    std.mem.writeInt(u16, buf[4..6], 12, .little);
    var sum: u16 = 0;
    for (buf[0 .. 12 + 8]) |byte| sum +%= byte;
    std.mem.writeInt(u16, buf[6..8], sum, .little);
    return buf;
}

test "null handle or counters is a null-pointer error" {
    var link = openLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    try std.testing.expectEqual(@as(u16, 0x504), pump_abi.priv_c6link_pump(null, 1, &stats));
    try std.testing.expectEqual(@as(u16, 0x504), pump_abi.priv_c6link_pump(&link, 1, null));
}

test "a data frame reaches the dispatcher with the counters published" {
    bench = .{ .reply = sealedReply() };
    var link = openLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    try std.testing.expectEqual(@as(u16, 0), pump_abi.priv_c6link_pump(&link, 4, &stats));
    try std.testing.expectEqual(@as(usize, 1), bench.dispatched);
    try std.testing.expect(bench.stats_seen);
    try std.testing.expect(link.stats == null);
    try std.testing.expectEqual(@as(u16, 1), stats.transfers);
    try std.testing.expectEqual(@as(u16, 1), stats.data);
}

test "a staged payload is consumed by the first transaction" {
    bench = .{};
    var link = openLink();
    link.tx_len = 24;
    link.tx_if = 2;
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    _ = pump_abi.priv_c6link_pump(&link, 1, &stats);
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
    try std.testing.expectEqual(@as(u16, 24), std.mem.readInt(u16, link.tx[2..4], .little));
}

test "a transport fault is a bus error and an absent peer a timeout" {
    bench = .{ .fail = true };
    var link = openLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    try std.testing.expectEqual(@as(u16, 0x402), pump_abi.priv_c6link_pump(&link, 4, &stats));
    try std.testing.expectEqual(@as(u16, 0), stats.transfers);

    bench = .{ .armed = false };
    stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    try std.testing.expectEqual(@as(u16, 0x203), pump_abi.priv_c6link_pump(&link, 64, &stats));
    try std.testing.expectEqual(@as(u16, 3), stats.hs_timeouts);
    try std.testing.expect(link.stats == null);
}
