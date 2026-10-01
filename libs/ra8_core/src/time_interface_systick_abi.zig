//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The SysTick binding of the injectable time interface.
//!
//! `libs/ra8_core/inc/ra8_time_interface.h` publishes a two-call vtable so a
//! caller can be handed a fake clock in a test. This file is the production
//! binding: it forwards to `ra8_time_ms` and `ra8_delay_ms`, Zig themselves
//! since #2851, and carries no context of its own.

/// Layout of `ra8_time_interface_t`.
const TimeInterface = extern struct {
    now_ms: *const fn (ctx: ?*anyopaque) callconv(.c) u32,
    delay_ms: *const fn (ctx: ?*anyopaque, ms: u32) callconv(.c) void,
    ctx: ?*anyopaque,
};

extern fn ra8_time_ms() u32;
extern fn ra8_delay_ms(ms: u32) void;

fn nowMs(ctx: ?*anyopaque) callconv(.c) u32 {
    _ = ctx;
    return ra8_time_ms();
}

fn delayMs(ctx: ?*anyopaque, ms: u32) callconv(.c) void {
    _ = ctx;
    ra8_delay_ms(ms);
}

export const g_ra8_time_interface_systick: TimeInterface = .{
    .now_ms = &nowMs,
    .delay_ms = &delayMs,
    .ctx = null,
};
