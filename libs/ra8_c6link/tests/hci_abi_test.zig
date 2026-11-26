//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_set_hci_rx` and `ra8_c6link_hci_send` against a scripted pump
//! supplied here through the C ABI.

const std = @import("std");
const hci = @import("hci_abi");

const c = hci.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
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
    staged_pkt_type: u8 = 0,
    staged: [4]u8 = .{ 0, 0, 0, 0 },
};

var script: Script = .{};

export fn priv_c6link_pump(link: ?*c.ra8_c6link_t, max_transactions: u16, _: ?*c.ra8_c6link_stats_t) callconv(.c) u16 {
    const handle = link.?;
    script.pumps += 1;
    script.budget = max_transactions;
    script.staged_len = handle.tx_len;
    script.staged_if = handle.tx_if;
    script.staged_pkt_type = handle.tx_pkt_type;
    const at = c.k_ra8_c6link_header_bytes;
    const copy_len = @min(script.staged.len, handle.tx_len);
    @memcpy(script.staged[0..copy_len], handle.tx[at .. at + copy_len]);
    if (script.drain) {
        handle.tx_len = 0;
        handle.tx_pkt_type = 0;
    }
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

fn onHciRx(ctx: ?*anyopaque, pkt: [*c]const u8, len: u16) callconv(.c) void {
    _ = ctx;
    _ = pkt;
    _ = len;
}

const hci_cmd = [_]u8{ 0x01, 0x03, 0x0C, 0x00 };
const hci_acl = [_]u8{ 0x02, 0x01, 0x20, 0x02, 0x00 };

test "set_hci_rx validates link state and attaches or clears context" {
    var link = openLink();
    var marker: u8 = 0;
    try std.testing.expectEqual(Code.null_ptr, code(hci.ra8_c6link_set_hci_rx(null, onHciRx, &marker)));
    link.open = false;
    try std.testing.expectEqual(Code.not_initialized, code(hci.ra8_c6link_set_hci_rx(&link, onHciRx, &marker)));
    link.open = true;
    try std.testing.expectEqual(Code.ok, code(hci.ra8_c6link_set_hci_rx(&link, onHciRx, &marker)));
    try std.testing.expect(link.hci_rx_cb != null);
    try std.testing.expectEqual(@as(?*anyopaque, &marker), link.hci_ctx);
    try std.testing.expectEqual(Code.ok, code(hci.ra8_c6link_set_hci_rx(&link, null, &marker)));
    try std.testing.expect(link.hci_rx_cb == null);
    try std.testing.expect(link.hci_ctx == null);
}

test "an H4 command stages pkt_type in handle and body after header" {
    script = .{};
    var link = openLink();
    try std.testing.expectEqual(Code.ok, code(hci.ra8_c6link_hci_send(&link, &hci_cmd, hci_cmd.len)));
    try std.testing.expectEqual(@as(usize, 1), script.pumps);
    try std.testing.expectEqual(@as(u16, c.k_ra8_c6link_hs_giveup), script.budget);
    try std.testing.expectEqual(@as(u16, hci_cmd.len - 1), script.staged_len);
    try std.testing.expectEqual(@as(u8, 4), script.staged_if);
    try std.testing.expectEqual(@as(u8, 0x01), script.staged_pkt_type);
    try std.testing.expectEqualSlices(u8, hci_cmd[1..], script.staged[0 .. hci_cmd.len - 1]);
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
    try std.testing.expectEqual(@as(u8, 0), link.tx_pkt_type);
}

test "ACL, SCO, ISO and max-length H4 packets are admitted" {
    var link = openLink();
    var max_pkt: [c.k_ra8_c6link_hci_max_packet]u8 = @splat(0x5A);
    max_pkt[0] = 0x02;

    script = .{};
    try std.testing.expectEqual(Code.ok, code(hci.ra8_c6link_hci_send(&link, &hci_acl, hci_acl.len)));
    try std.testing.expectEqual(@as(u8, 0x02), script.staged_pkt_type);

    const sco = [_]u8{ 0x03, 0x11, 0x22 };
    script = .{};
    try std.testing.expectEqual(Code.ok, code(hci.ra8_c6link_hci_send(&link, &sco, sco.len)));
    try std.testing.expectEqual(@as(u8, 0x03), script.staged_pkt_type);

    const iso = [_]u8{ 0x05, 0x33 };
    script = .{};
    try std.testing.expectEqual(Code.ok, code(hci.ra8_c6link_hci_send(&link, &iso, iso.len)));
    try std.testing.expectEqual(@as(u8, 0x05), script.staged_pkt_type);

    script = .{};
    try std.testing.expectEqual(Code.ok, code(hci.ra8_c6link_hci_send(&link, &max_pkt, max_pkt.len)));
    try std.testing.expectEqual(@as(u16, c.k_ra8_c6link_max_payload), script.staged_len);
}

test "pump faults and unsent timeouts clear tx_len and tx_pkt_type" {
    script = .{ .verdict = Code.spi_error, .drain = false };
    var link = openLink();
    try std.testing.expectEqual(Code.spi_error, code(hci.ra8_c6link_hci_send(&link, &hci_cmd, hci_cmd.len)));
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
    try std.testing.expectEqual(@as(u8, 0), link.tx_pkt_type);

    script = .{ .drain = false };
    try std.testing.expectEqual(Code.hw_timeout, code(hci.ra8_c6link_hci_send(&link, &hci_cmd, hci_cmd.len)));
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
    try std.testing.expectEqual(@as(u8, 0), link.tx_pkt_type);
}

test "nulls, closed link, bad length, invalid H4 type and busy slot are refused cleanly" {
    script = .{};
    var link = openLink();
    const bad_evt = [_]u8{ 0x04, 0x0E, 0x04 };
    const bad_zero = [_]u8{ 0x00, 0x01 };
    try std.testing.expectEqual(Code.null_ptr, code(hci.ra8_c6link_hci_send(null, &hci_cmd, hci_cmd.len)));
    try std.testing.expectEqual(Code.null_ptr, code(hci.ra8_c6link_hci_send(&link, null, hci_cmd.len)));
    try std.testing.expectEqual(Code.invalid_size, code(hci.ra8_c6link_hci_send(&link, &hci_cmd, 0)));
    try std.testing.expectEqual(Code.invalid_size, code(hci.ra8_c6link_hci_send(&link, &hci_cmd, 1)));
    try std.testing.expectEqual(
        Code.invalid_size,
        code(hci.ra8_c6link_hci_send(&link, &hci_cmd, c.k_ra8_c6link_hci_max_packet + 1)),
    );
    try std.testing.expectEqual(Code.invalid_arg, code(hci.ra8_c6link_hci_send(&link, &bad_evt, bad_evt.len)));
    try std.testing.expectEqual(Code.invalid_arg, code(hci.ra8_c6link_hci_send(&link, &bad_zero, bad_zero.len)));
    link.tx_len = 9;
    link.tx_pkt_type = 0x02;
    try std.testing.expectEqual(Code.busy, code(hci.ra8_c6link_hci_send(&link, &hci_cmd, hci_cmd.len)));
    try std.testing.expectEqual(@as(u16, 9), link.tx_len);
    try std.testing.expectEqual(@as(u8, 0x02), link.tx_pkt_type);
    link.tx_len = 0;
    link.tx_pkt_type = 0;
    link.open = false;
    try std.testing.expectEqual(Code.not_initialized, code(hci.ra8_c6link_hci_send(&link, &hci_cmd, hci_cmd.len)));
    try std.testing.expectEqual(@as(usize, 0), script.pumps);
}
