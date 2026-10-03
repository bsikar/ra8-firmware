//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_open`, `ra8_c6link_close`, `ra8_c6link_is_open` and
//! `ra8_c6link_last_fault` on a real `ra8_c6link_t`.

const std = @import("std");
const lifecycle = @import("lifecycle_abi");

const c = lifecycle.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const invalid_state: u16 = 0x104;
    pub const invalid_size: u16 = 0x105;
    pub const not_initialized: u16 = 0x10F;
    pub const null_ptr: u16 = 0x504;
};

var arena: [c.k_ra8_c6link_arena_min]u8 = undefined;
var marker: u8 = 0;

fn transfer(_: ?*anyopaque, _: [*c]const u8, _: [*c]u8, _: u16) callconv(.c) c.ra8_err_t {
    return Code.ok;
}

fn handshakeActive(_: ?*anyopaque) callconv(.c) bool {
    return false;
}

fn delayMs(_: ?*anyopaque, _: u16) callconv(.c) void {}

fn onEvent(_: ?*anyopaque, _: [*c]const c.ra8_c6link_event_t) callconv(.c) void {}

fn goodCfg() c.ra8_c6link_cfg_t {
    var cfg = std.mem.zeroes(c.ra8_c6link_cfg_t);
    cfg.transport.transfer = transfer;
    cfg.transport.handshake_active = handshakeActive;
    cfg.transport.delay_ms = delayMs;
    cfg.arena = &arena;
    cfg.arena_bytes = arena.len;
    cfg.event_cb = onEvent;
    cfg.cb_ctx = &marker;
    return cfg;
}

fn code(err: c.ra8_err_t) u16 {
    return @intCast(err);
}

test "open binds the configuration and starts a clean session" {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    link.arena_used = 7;
    link.arena_last = 3;
    link.next_uid = 9;
    link.wait.armed = true;
    link.fault.rpc_id = 5;
    link.stats = &stats;
    link.tx_len = 12;
    link.tx_if = 2;
    link.boot_seen = true;
    const cfg = goodCfg();
    try std.testing.expectEqual(Code.ok, code(lifecycle.ra8_c6link_open(&link, &cfg)));
    try std.testing.expect(link.open);
    try std.testing.expect(lifecycle.ra8_c6link_is_open(&link));
    try std.testing.expectEqual(@as([*c]u8, &arena), link.arena);
    try std.testing.expectEqual(@as(u32, arena.len), link.arena_bytes);
    try std.testing.expectEqual(@as(?*anyopaque, &marker), link.cb_ctx);
    try std.testing.expect(link.event_cb != null and link.rx_cb == null);
    try std.testing.expect(link.transport.transfer != null);
    try std.testing.expectEqual(@as(u32, 0), link.arena_used + link.arena_last + link.next_uid);
    try std.testing.expect(!link.wait.armed and link.fault.rpc_id == 0);
    try std.testing.expect(link.stats == null);
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
    try std.testing.expectEqual(@as(u8, 0), link.tx_if);
    try std.testing.expect(!link.boot_seen);
}

test "open refuses null arguments and an already-open handle" {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    const cfg = goodCfg();
    try std.testing.expectEqual(Code.null_ptr, code(lifecycle.ra8_c6link_open(null, &cfg)));
    try std.testing.expectEqual(Code.null_ptr, code(lifecycle.ra8_c6link_open(&link, null)));
    try std.testing.expectEqual(Code.ok, code(lifecycle.ra8_c6link_open(&link, &cfg)));
    try std.testing.expectEqual(Code.invalid_state, code(lifecycle.ra8_c6link_open(&link, &cfg)));
}

test "open refuses each missing transport row, a missing arena and a small arena" {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    var cfg = goodCfg();
    cfg.transport.transfer = null;
    try std.testing.expectEqual(Code.null_ptr, code(lifecycle.ra8_c6link_open(&link, &cfg)));
    cfg = goodCfg();
    cfg.transport.handshake_active = null;
    try std.testing.expectEqual(Code.null_ptr, code(lifecycle.ra8_c6link_open(&link, &cfg)));
    cfg = goodCfg();
    cfg.transport.delay_ms = null;
    try std.testing.expectEqual(Code.null_ptr, code(lifecycle.ra8_c6link_open(&link, &cfg)));
    cfg = goodCfg();
    cfg.arena = null;
    try std.testing.expectEqual(Code.null_ptr, code(lifecycle.ra8_c6link_open(&link, &cfg)));
    cfg = goodCfg();
    cfg.arena_bytes = c.k_ra8_c6link_arena_min - 1;
    try std.testing.expectEqual(Code.invalid_size, code(lifecycle.ra8_c6link_open(&link, &cfg)));
    try std.testing.expect(!link.open);
}

test "close forgets the session but keeps the fault, and the handle reopens" {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    const cfg = goodCfg();
    try std.testing.expectEqual(Code.ok, code(lifecycle.ra8_c6link_open(&link, &cfg)));
    link.stats = &stats;
    link.tx_len = 4;
    link.wait.armed = true;
    link.fault.rpc_id = 11;
    try std.testing.expectEqual(Code.ok, code(lifecycle.ra8_c6link_close(&link)));
    try std.testing.expect(!lifecycle.ra8_c6link_is_open(&link));
    try std.testing.expect(link.transport.transfer == null and link.event_cb == null);
    try std.testing.expect(link.cb_ctx == null and link.arena == null and link.stats == null);
    try std.testing.expect(link.tx_len == 0 and !link.wait.armed);
    try std.testing.expectEqual(@as(u32, 11), link.fault.rpc_id);
    try std.testing.expectEqual(Code.not_initialized, code(lifecycle.ra8_c6link_close(&link)));
    try std.testing.expectEqual(Code.ok, code(lifecycle.ra8_c6link_open(&link, &cfg)));
}

test "close and is_open on a null handle" {
    try std.testing.expectEqual(Code.null_ptr, code(lifecycle.ra8_c6link_close(null)));
    try std.testing.expect(!lifecycle.ra8_c6link_is_open(null));
}

test "last_fault copies the recorded fault and refuses nulls" {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.fault.rpc_id = 0x101;
    link.fault.resp = -3;
    var out = std.mem.zeroes(c.ra8_c6link_fault_t);
    try std.testing.expectEqual(Code.ok, code(lifecycle.ra8_c6link_last_fault(&link, &out)));
    try std.testing.expectEqual(@as(u32, 0x101), out.rpc_id);
    try std.testing.expectEqual(@as(i32, -3), out.resp);
    try std.testing.expectEqual(Code.null_ptr, code(lifecycle.ra8_c6link_last_fault(null, &out)));
    try std.testing.expectEqual(Code.null_ptr, code(lifecycle.ra8_c6link_last_fault(&link, null)));
}
