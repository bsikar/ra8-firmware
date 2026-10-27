//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The stub UART profile, and nothing else.
//!
//! `if_stub` is the proof binding of epic RA8FW-618 slice 5: the `if_<chip>`
//! set a second target supplies in place of the RA8 adapters. This unit is
//! that set's UART member. It reads no hardware and touches no register, so
//! the whole profile is a pure function of its call history and runs on the
//! host without a fake peripheral block.
//!
//! The profile is deliberately tiny and deliberately arbitrary: two lines in
//! dense board numbering, a console (UART 0) and a modem (UART 1), each
//! coming up at the stub board's firmware default of 115200 8N1. A write is
//! captured into a per-line ring, and a read drains a per-line receive queue
//! a test primes, so the port's configure, transmit and poll-read intents run
//! end to end through the facade while the profile asserts nothing about real
//! silicon. A real chip port replaces this file, and the interface above it
//! never changes.
//!
//! The enums are spelled numerically so this unit stays free of the C header;
//! the smoke translation unit pins them against `fw_if_uart.h` itself, which
//! is what keeps the two transcriptions from drifting apart.

/// `fw_uart_data_bits_t` values the profile names.
pub const data_bits_none: u8 = 0; // k_fw_uart_data_bits_none
pub const data_bits_7: u8 = 1; // k_fw_uart_data_bits_7
pub const data_bits_8: u8 = 2; // k_fw_uart_data_bits_8

/// `fw_uart_parity_t` values the profile names.
pub const parity_none: u8 = 0; // k_fw_uart_parity_none
pub const parity_even: u8 = 1; // k_fw_uart_parity_even
pub const parity_odd: u8 = 2; // k_fw_uart_parity_odd

/// `fw_uart_stop_bits_t` values the profile names.
pub const stop_bits_none: u8 = 0; // k_fw_uart_stop_bits_none
pub const stop_bits_one: u8 = 1; // k_fw_uart_stop_bits_one
pub const stop_bits_two: u8 = 2; // k_fw_uart_stop_bits_two

/// How many lines the stub board carries: 0 console, 1 modem, dense.
pub const uart_count: usize = 2;

/// Receive bytes each line holds before a feed starts dropping.
pub const rx_capacity: usize = 16;

/// Transmit bytes each line's ring holds before it wraps.
pub const tx_capacity: usize = 64;

/// One line the stub board carries.
const Line = struct {
    baud: u32 = 115200,
    data_bits: u8 = data_bits_8,
    parity: u8 = parity_none,
    stop_bits: u8 = stop_bits_one,
    rx: [rx_capacity]u8 = undefined,
    rx_len: usize = 0,
    rx_head: usize = 0,
    tx: [tx_capacity]u8 = undefined,
    tx_head: usize = 0,
    sent: usize = 0,
};

var lines = [_]Line{ .{}, .{} };

/// The line parameters a configure request names.
pub const Request = struct {
    baud: u32,
    data_bits: u8,
    parity: u8,
    stop_bits: u8,
};

/// Is this line one the stub board carries?
pub fn carries(uart: u8) bool {
    return uart < uart_count;
}

/// Put one line on a line description. A zero baud is not a line, and an
/// out-of-range field is refused, the way the port's own contract describes
/// the request.
pub fn configure(uart: u8, request: Request) error{ InvalidArg, NotFound }!void {
    const slot = slot_of(uart) orelse return error.NotFound;

    if (request.baud == 0) return error.InvalidArg;
    if (request.data_bits == data_bits_none or request.data_bits > data_bits_8) return error.InvalidArg;
    if (request.parity > parity_odd) return error.InvalidArg;
    if (request.stop_bits == stop_bits_none or request.stop_bits > stop_bits_two) return error.InvalidArg;

    lines[slot].baud = request.baud;
    lines[slot].data_bits = request.data_bits;
    lines[slot].parity = request.parity;
    lines[slot].stop_bits = request.stop_bits;
}

/// Transmit bytes on one line, captured into its ring in order. A
/// zero-length write is satisfied without waking the peripheral, the way the
/// port's own contract describes the request.
pub fn write(uart: u8, bytes: []const u8) error{NotFound}!void {
    const slot = slot_of(uart) orelse return error.NotFound;

    if (bytes.len == 0) return;

    for (bytes) |byte| {
        lines[slot].tx[lines[slot].tx_head] = byte;
        lines[slot].tx_head = (lines[slot].tx_head + 1) % tx_capacity;
    }
    lines[slot].sent += bytes.len;
}

/// Poll one line for received bytes, up to `out.len`; a quiet line reads
/// zero. The receive queue is primed through `feed`, a test seam.
pub fn read(uart: u8, out: []u8) error{NotFound}!usize {
    const slot = slot_of(uart) orelse return error.NotFound;

    var count: usize = 0;
    while (count < out.len and lines[slot].rx_len > 0) : (count += 1) {
        out[count] = lines[slot].rx[lines[slot].rx_head];
        lines[slot].rx_head = (lines[slot].rx_head + 1) % rx_capacity;
        lines[slot].rx_len -= 1;
    }

    return count;
}

/// Prime a line's receive queue. Overflow past `rx_capacity` is dropped;
/// priming is a test seam, not product surface.
pub fn feed(uart: u8, bytes: []const u8) error{NotFound}!void {
    const slot = slot_of(uart) orelse return error.NotFound;

    for (bytes) |byte| {
        if (lines[slot].rx_len == rx_capacity) break;
        lines[slot].rx[(lines[slot].rx_head + lines[slot].rx_len) % rx_capacity] = byte;
        lines[slot].rx_len += 1;
    }
}

/// The parameters a line currently runs; tests observe them.
pub fn line(uart: u8) ?Line {
    const slot = slot_of(uart) orelse return null;
    return lines[slot];
}

/// How many bytes a line has transmitted since reset; tests observe it.
pub fn transmitted(uart: u8) ?usize {
    const slot = slot_of(uart) orelse return null;
    return lines[slot].sent;
}

fn slot_of(uart: u8) ?usize {
    if (!carries(uart)) return null;
    return @as(usize, uart);
}

/// Restore the default board. Test hygiene, not product surface.
pub fn reset() void {
    lines = [_]Line{ .{}, .{} };
}
