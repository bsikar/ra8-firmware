//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The debug console against a fake SCI. The two things that have actually
//! bitten here are the clock the BRR is computed from and the readiness gate,
//! so both get held down: PCLKA is read live and handed to the SCI unchanged,
//! a tree that has not left MOCO is refused, and nothing reaches the SCI
//! before a successful init.

const std = @import("std");
const uart = @import("uart_console");

const SciCfg = extern struct {
    baud: u32,
    data_bits: u8,
    parity: u8,
    stop_bits: u8,
    pclk_hz: u32,
};

var pclka_hz: u32 = 125_000_000;
var clock_result: u32 = 0;
var route_result: u32 = 0;
var routed: [4]u16 = undefined;
var routed_len: usize = 0;
var sci_result: u32 = 0;
var last_cfg: SciCfg = undefined;
var written: [32]u8 = undefined;
var written_len: usize = 0;
var rx_queue: []const u8 = &.{};
var rx_taken: usize = 0;
var flush_calls: usize = 0;

export fn ra8_cgc_get_clock_hz(id: u32, out_hz: *u32) u32 {
    _ = id;
    out_hz.* = pclka_hz;
    return clock_result;
}

export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    _ = psel;
    _ = owner;
    routed[routed_len] = pin;
    routed_len += 1;
    return route_result;
}

export fn ra8_sci_init(channel: u8, cfg: *const SciCfg) u32 {
    _ = channel;
    last_cfg = cfg.*;
    return sci_result;
}

export fn ra8_sci_write_polling(channel: u8, data: [*]const u8, len: u32) u32 {
    _ = channel;
    for (data[0..len]) |byte| {
        written[written_len] = byte;
        written_len += 1;
    }
    return 0;
}

export fn ra8_sci_getc_polling(channel: u8, out_byte: *u8) u32 {
    _ = channel;
    if (rx_taken >= rx_queue.len) return 0x10B;
    out_byte.* = rx_queue[rx_taken];
    rx_taken += 1;
    return 0;
}

export fn ra8_sci_flush(channel: u8) u32 {
    _ = channel;
    flush_calls += 1;
    return 0;
}

fn reset() void {
    pclka_hz = 125_000_000;
    clock_result = 0;
    route_result = 0;
    routed_len = 0;
    sci_result = 0;
    written_len = 0;
    rx_queue = &.{};
    rx_taken = 0;
    flush_calls = 0;
}

/// Puts the console in the state a board that booted properly would leave it.
fn bringUp() !void {
    reset();
    try std.testing.expectEqual(@as(u32, 0), uart.init(115200));
}

test "a zero baud is refused before anything is read or routed" {
    reset();
    try std.testing.expectEqual(@as(u32, 0x103), uart.init(0));
    try std.testing.expectEqual(@as(usize, 0), routed_len);
}

test "init routes TXD then RXD and hands the live PCLKA to the SCI" {
    try bringUp();
    try std.testing.expectEqual(@as(usize, 2), routed_len);
    try std.testing.expectEqual(uart.Console.pin_txd, routed[0]);
    try std.testing.expectEqual(uart.Console.pin_rxd, routed[1]);
    try std.testing.expectEqual(@as(u32, 115200), last_cfg.baud);
    try std.testing.expectEqual(@as(u32, 125_000_000), last_cfg.pclk_hz);
    try std.testing.expectEqual(@as(u8, 8), last_cfg.data_bits);
    try std.testing.expectEqual(@as(u8, 0), last_cfg.parity);
    try std.testing.expectEqual(@as(u8, 0), last_cfg.stop_bits);
    try std.testing.expect(uart.isUp());
}

test "a BRR computed from whatever CGC settled on, not from a constant" {
    reset();
    pclka_hz = 48_000_000;
    try std.testing.expectEqual(@as(u32, 0), uart.init(115200));
    try std.testing.expectEqual(@as(u32, 48_000_000), last_cfg.pclk_hz);
}

test "a tree still on MOCO is refused and nothing is routed" {
    reset();
    pclka_hz = 8_000_000;
    try std.testing.expectEqual(@as(u32, 0x10F), uart.init(115200));
    try std.testing.expectEqual(@as(usize, 0), routed_len);
}

test "a failed pin route stops the init before the SCI is configured" {
    reset();
    route_result = 0x106;
    try std.testing.expectEqual(@as(u32, 0x106), uart.init(115200));
    try std.testing.expectEqual(@as(usize, 1), routed_len);
}

test "a failed SCI init propagates" {
    reset();
    sci_result = 0x10B;
    try std.testing.expectEqual(@as(u32, 0x10B), uart.init(115200));
}

test "write forwards the whole slice once the console is up" {
    try bringUp();
    try std.testing.expectEqual(@as(u32, 0), uart.write("hi"));
    try std.testing.expectEqualStrings("hi", written[0..written_len]);
}

test "read drains what the SCI has and stops at the first empty poll" {
    try bringUp();
    rx_queue = "ab";
    var buf: [4]u8 = undefined;
    const result = uart.read(&buf);
    try std.testing.expectEqual(@as(u32, 0), result.err);
    try std.testing.expectEqual(@as(usize, 2), result.filled);
    try std.testing.expectEqualStrings("ab", buf[0..result.filled]);
}

test "read fills no more than the slice it was given" {
    try bringUp();
    rx_queue = "abcdef";
    var buf: [3]u8 = undefined;
    const result = uart.read(&buf);
    try std.testing.expectEqual(@as(usize, 3), result.filled);
    try std.testing.expectEqualStrings("abc", buf[0..result.filled]);
}

test "nothing available is ok with an empty prefix, not an error" {
    try bringUp();
    var buf: [4]u8 = undefined;
    const result = uart.read(&buf);
    try std.testing.expectEqual(@as(u32, 0), result.err);
    try std.testing.expectEqual(@as(usize, 0), result.filled);
}

test "flush defers to the SCI once the console is up" {
    try bringUp();
    try std.testing.expectEqual(@as(u32, 0), uart.flush());
    try std.testing.expectEqual(@as(usize, 1), flush_calls);
}

test "the console lives on SCI8 at PD02/PD03 with a 16 MHz PCLKA floor" {
    try std.testing.expectEqual(@as(u8, 8), uart.Console.sci_channel);
    try std.testing.expectEqual(@as(u16, 0x0D02), uart.Console.pin_txd);
    try std.testing.expectEqual(@as(u16, 0x0D03), uart.Console.pin_rxd);
    try std.testing.expectEqual(@as(u32, 16_000_000), uart.Console.min_pclka_hz);
}
