//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_eth_send` against a scripted pump supplied here through the
//! C ABI.

const std = @import("std");
const eth = @import("eth_abi");

const c = eth.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const invalid_size: u16 = 0x105;
    pub const busy: u16 = 0x109;
    pub const not_initialized: u16 = 0x10F;
    pub const hw_timeout: u16 = 0x203;
    pub const spi_error: u16 = 0x402;
    pub const null_ptr: u16 = 0x504;
};

const Script = struct {
    verdict: u16 = Code.ok,
    drain: bool = true,
    pumps: usize = 0,
    budget: u16 = 0,
    staged_len: u16 = 0,
    staged_if: u8 = 0,
    staged: [4]u8 = .{ 0, 0, 0, 0 },
};

var script: Script = .{};

export fn priv_c6link_pump(link: ?*c.ra8_c6link_t, max_transactions: u16, _: ?*c.ra8_c6link_stats_t) callconv(.c) u16 {
    const handle = link.?;
    script.pumps += 1;
    script.budget = max_transactions;
    script.staged_len = handle.tx_len;
    script.staged_if = handle.tx_if;
    const at = c.k_ra8_c6link_header_bytes;
    @memcpy(&script.staged, handle.tx[at .. at + 4]);
    if (script.drain) handle.tx_len = 0;
    return script.verdict;
}

fn code(err: c.ra8_err_t) u16 {
    return @intCast(err);
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    return link;
}

const payload = [_]u8{ 0xDE, 0xAD, 0xBE, 0xEF };

test "a frame is staged after the header on the station interface and sent" {
    script = .{};
    var link = openLink();
    try std.testing.expectEqual(Code.ok, code(eth.ra8_c6link_eth_send(&link, &payload, payload.len)));
    try std.testing.expectEqual(@as(usize, 1), script.pumps);
    try std.testing.expectEqual(@as(u16, c.k_ra8_c6link_hs_giveup), script.budget);
    try std.testing.expectEqual(@as(u16, payload.len), script.staged_len);
    try std.testing.expectEqual(@as(u8, 1), script.staged_if);
    try std.testing.expectEqualSlices(u8, &payload, &script.staged);
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
}

test "a pump fault is returned and the slot is cleared" {
    script = .{ .verdict = Code.spi_error, .drain = false };
    var link = openLink();
    try std.testing.expectEqual(Code.spi_error, code(eth.ra8_c6link_eth_send(&link, &payload, payload.len)));
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
}

test "a frame still staged after the budget is a timeout and the slot is cleared" {
    script = .{ .drain = false };
    var link = openLink();
    try std.testing.expectEqual(Code.hw_timeout, code(eth.ra8_c6link_eth_send(&link, &payload, payload.len)));
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
}

test "nulls, a closed link, a bad length and a busy slot are refused before the pump" {
    script = .{};
    var link = openLink();
    try std.testing.expectEqual(Code.null_ptr, code(eth.ra8_c6link_eth_send(null, &payload, payload.len)));
    try std.testing.expectEqual(Code.null_ptr, code(eth.ra8_c6link_eth_send(&link, null, payload.len)));
    try std.testing.expectEqual(Code.invalid_size, code(eth.ra8_c6link_eth_send(&link, &payload, 0)));
    try std.testing.expectEqual(Code.invalid_size, code(eth.ra8_c6link_eth_send(&link, &payload, c.k_ra8_c6link_max_payload + 1)));
    link.tx_len = 9;
    try std.testing.expectEqual(Code.busy, code(eth.ra8_c6link_eth_send(&link, &payload, payload.len)));
    try std.testing.expectEqual(@as(u16, 9), link.tx_len);
    link.tx_len = 0;
    link.open = false;
    try std.testing.expectEqual(Code.not_initialized, code(eth.ra8_c6link_eth_send(&link, &payload, payload.len)));
    try std.testing.expectEqual(@as(usize, 0), script.pumps);
}
