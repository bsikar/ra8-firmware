//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_poll` and `ra8_c6link_await_ready` against a scripted pump
//! and identity request, both supplied here through the C ABI.

const std = @import("std");
const ready = @import("ready_abi");

const c = ready.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const busy: u16 = 0x109;
    pub const not_initialized: u16 = 0x10F;
    pub const hw_timeout: u16 = 0x203;
    pub const spi_error: u16 = 0x402;
    pub const null_ptr: u16 = 0x504;
};

const Script = struct {
    verdicts: [4]u16 = .{ Code.ok, Code.ok, Code.ok, Code.ok },
    pumps: usize = 0,
    budget: u16 = 0,
    staged_len: u16 = 0,
    staged_if: u8 = 0,
    fw_calls: usize = 0,
    fw_verdict: u16 = Code.ok,
};

var script: Script = .{};

export fn priv_c6link_pump(link: ?*c.ra8_c6link_t, max_transactions: u16, stats: ?*c.ra8_c6link_stats_t) callconv(.c) u16 {
    const handle = link.?;
    script.staged_len = handle.tx_len;
    script.staged_if = handle.tx_if;
    script.budget = max_transactions;
    stats.?.transfers = 3;
    const verdict = script.verdicts[@min(script.pumps, script.verdicts.len - 1)];
    script.pumps += 1;
    return verdict;
}

export fn ra8_c6link_fw_version(_: ?*c.ra8_c6link_t, out: ?*c.ra8_c6link_fw_version_t) callconv(.c) c.ra8_err_t {
    script.fw_calls += 1;
    out.?.* = std.mem.zeroes(c.ra8_c6link_fw_version_t);
    return @intCast(script.fw_verdict);
}

fn code(err: c.ra8_err_t) u16 {
    return @intCast(err);
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    return link;
}

test "poll pumps with local counters and copies them out" {
    script = .{};
    var link = openLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    try std.testing.expectEqual(Code.ok, code(ready.ra8_c6link_poll(&link, 7, &stats)));
    try std.testing.expectEqual(@as(u16, 7), script.budget);
    try std.testing.expectEqual(@as(u16, 3), stats.transfers);
    try std.testing.expectEqual(Code.ok, code(ready.ra8_c6link_poll(&link, 1, null)));
    script.verdicts[2] = Code.spi_error;
    try std.testing.expectEqual(Code.spi_error, code(ready.ra8_c6link_poll(&link, 1, null)));
}

test "poll refuses a null or closed handle and a zero budget" {
    script = .{};
    var link = openLink();
    try std.testing.expectEqual(Code.null_ptr, code(ready.ra8_c6link_poll(null, 1, null)));
    try std.testing.expectEqual(Code.invalid_arg, code(ready.ra8_c6link_poll(&link, 0, null)));
    link.open = false;
    try std.testing.expectEqual(Code.not_initialized, code(ready.ra8_c6link_poll(&link, 1, null)));
    try std.testing.expectEqual(@as(usize, 0), script.pumps);
}

test "await_ready announces on the privileged interface, then asks for the version" {
    script = .{};
    var link = openLink();
    var version: c.ra8_c6link_fw_version_t = undefined;
    try std.testing.expectEqual(Code.ok, code(ready.ra8_c6link_await_ready(&link, 5, &version)));
    try std.testing.expectEqual(@as(usize, 1), script.pumps);
    try std.testing.expectEqual(@as(u16, 17), script.staged_len);
    try std.testing.expectEqual(@as(u8, 5), script.staged_if);
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
    try std.testing.expectEqual(@as(usize, 1), script.fw_calls);
}

test "await_ready retries a quiet co-processor and gives up after the attempts" {
    script = .{ .verdicts = .{ Code.hw_timeout, Code.ok, Code.ok, Code.ok } };
    var link = openLink();
    var version: c.ra8_c6link_fw_version_t = undefined;
    try std.testing.expectEqual(Code.ok, code(ready.ra8_c6link_await_ready(&link, 5, &version)));
    try std.testing.expectEqual(@as(usize, 2), script.pumps);

    script = .{ .verdicts = .{ Code.hw_timeout, Code.hw_timeout, Code.hw_timeout, Code.hw_timeout } };
    try std.testing.expectEqual(Code.hw_timeout, code(ready.ra8_c6link_await_ready(&link, 5, &version)));
    try std.testing.expectEqual(@as(usize, c.k_ra8_c6link_ready_attempts), script.pumps);
    try std.testing.expectEqual(@as(usize, 0), script.fw_calls);
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
}

test "await_ready returns another fault at once and passes on the version verdict" {
    script = .{ .verdicts = .{ Code.spi_error, Code.ok, Code.ok, Code.ok } };
    var link = openLink();
    var version: c.ra8_c6link_fw_version_t = undefined;
    try std.testing.expectEqual(Code.spi_error, code(ready.ra8_c6link_await_ready(&link, 5, &version)));
    try std.testing.expectEqual(@as(usize, 1), script.pumps);
    try std.testing.expectEqual(@as(usize, 0), script.fw_calls);

    script = .{ .fw_verdict = Code.busy };
    try std.testing.expectEqual(Code.busy, code(ready.ra8_c6link_await_ready(&link, 5, &version)));
    try std.testing.expectEqual(@as(usize, 1), script.fw_calls);
}

test "await_ready refuses nulls, a closed handle, a zero budget and a staged payload" {
    script = .{};
    var link = openLink();
    var version: c.ra8_c6link_fw_version_t = undefined;
    try std.testing.expectEqual(Code.null_ptr, code(ready.ra8_c6link_await_ready(null, 1, &version)));
    try std.testing.expectEqual(Code.null_ptr, code(ready.ra8_c6link_await_ready(&link, 1, null)));
    try std.testing.expectEqual(Code.invalid_arg, code(ready.ra8_c6link_await_ready(&link, 0, &version)));
    link.tx_len = 4;
    try std.testing.expectEqual(Code.busy, code(ready.ra8_c6link_await_ready(&link, 1, &version)));
    link.tx_len = 0;
    link.open = false;
    try std.testing.expectEqual(Code.not_initialized, code(ready.ra8_c6link_await_ready(&link, 1, &version)));
    try std.testing.expectEqual(@as(usize, 0), script.pumps);
}
