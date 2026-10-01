//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host test double for the console's SCI seam. Records what the policy asked
//! for and replays a scripted receive queue, so the ordering and the gate are
//! checked without an MCU.

const Err = @import("console").vocab.Err;

/// Scripted state. One instance per test; reset with `clear`.
pub const state = struct {
    pub var pclka_hz: u32 = 100_000_000;
    pub var pclka_err: u32 = Err.ok;
    pub var route_err: u32 = Err.ok;
    pub var init_err: u32 = Err.ok;

    pub var routed: [2]u16 = .{ 0, 0 };
    pub var routed_count: usize = 0;
    pub var init_baud: u32 = 0;
    pub var init_pclk_hz: u32 = 0;
    pub var init_count: usize = 0;
    pub var written: usize = 0;
    pub var flushes: usize = 0;

    /// Bytes `getc` hands out, oldest first; it reports empty past the end.
    pub var rx: []const u8 = &.{};
    pub var rx_pos: usize = 0;

    pub fn clear() void {
        pclka_hz = 100_000_000;
        pclka_err = Err.ok;
        route_err = Err.ok;
        init_err = Err.ok;
        routed = .{ 0, 0 };
        routed_count = 0;
        init_baud = 0;
        init_pclk_hz = 0;
        init_count = 0;
        written = 0;
        flushes = 0;
        rx = &.{};
        rx_pos = 0;
    }
};

pub fn pclkaHz(out_hz: *u32) u32 {
    if (state.pclka_err != Err.ok) return state.pclka_err;
    out_hz.* = state.pclka_hz;
    return Err.ok;
}

pub fn route(pin: u16, psel: u8, owner: []const u8) u32 {
    _ = psel;
    _ = owner;
    if (state.route_err != Err.ok) return state.route_err;
    if (state.routed_count < state.routed.len) state.routed[state.routed_count] = pin;
    state.routed_count += 1;
    return Err.ok;
}

pub fn init(channel: u8, baud: u32, pclk_hz: u32) u32 {
    _ = channel;
    if (state.init_err != Err.ok) return state.init_err;
    state.init_baud = baud;
    state.init_pclk_hz = pclk_hz;
    state.init_count += 1;
    return Err.ok;
}

pub fn writePolling(channel: u8, data: []const u8) u32 {
    _ = channel;
    state.written += data.len;
    return Err.ok;
}

pub fn getc(channel: u8, out_byte: *u8) u32 {
    _ = channel;
    if (state.rx_pos >= state.rx.len) return Err.invalid_arg;
    out_byte.* = state.rx[state.rx_pos];
    state.rx_pos += 1;
    return Err.ok;
}

pub fn flush(channel: u8) u32 {
    _ = channel;
    state.flushes += 1;
    return Err.ok;
}
