//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the SPI_B transfer handler, Stop-mode and ISR dispatch calls,
//! plus the per-channel state table (RA8FW-894, was part of ra8_spi_b.c).
//! HUM Ch 43.2.4 "SPCR" p 2884, Ch 11.2.7 "MSTPCRB" p 444.

const std = @import("std");
const common = @import("abi_common.zig");
const clock = @import("internal/spi_b_clock.zig");
const events = @import("internal/spi_b_events.zig");

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

const channel_count: u8 = 2;
const bases = [channel_count]usize{ 0x4035_C000, 0x4035_C100 };
const mstp_ids = [channel_count]u16{ (1 << 8) | 19, (1 << 8) | 18 }; // MSTPB19 SPI0, MSTPB18 SPI1
const spcr_off = 0x08;
const spsr_off = 0x50;
const spsrc_off = 0x68;

/// Written by ra8_spi_init / ra8_spi_deinit in ra8_spi_b.c until they move.
export var s_spi_state: [channel_count]events.State = std.mem.zeroes([channel_count]events.State);

fn reg(channel: u8, off: usize) *volatile u32 {
    return @ptrFromInt(bases[channel] + off);
}

export fn ra8_spi_attach_transfer_handler(channel: u8, cb: ?events.CompleteFn, ctx: ?*anyopaque) u16 {
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    s_spi_state[channel].cb = cb;
    s_spi_state[channel].ctx = ctx;
    return common.k_ra8_ok;
}

/// Clear SPE, then gate the module clock.
export fn ra8_spi_enter_stop(channel: u8) u16 {
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    reg(channel, spcr_off).* = 0;
    return ra8_mstp_disable(mstp_ids[channel]);
}

export fn ra8_spi_exit_stop(channel: u8) u16 {
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    return ra8_mstp_enable(mstp_ids[channel]);
}

/// Placeholder until NVIC wiring routes SPTI; the C was a no-op too.
export fn ra8_spi_dispatch_spti(channel: u8) void {
    _ = channel;
}

/// Placeholder until NVIC wiring routes SPRI; the C was a no-op too.
export fn ra8_spi_dispatch_spri(channel: u8) void {
    _ = channel;
}

/// Snapshot the error flags, clear them through SPSRC, then report.
export fn ra8_spi_dispatch_spei(channel: u8) void {
    if (channel >= channel_count) return;
    const mask = clock.errMask(reg(channel, spsr_off).*);
    reg(channel, spsrc_off).* = clock.spsr_errs;
    events.report(s_spi_state[channel], mask);
}
