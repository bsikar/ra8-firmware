//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The console's SCI seam against the real HAL. Every register write happens
//! behind these entry points, each of which carries its own RA8P1 HUM citation
//! in the HAL; the board layer never touches a register itself.

extern fn ra8_cgc_get_clock_hz(id: u8, out_hz: *u32) u32;
extern fn ra8_pfs_route_peripheral(pin: u16, psel: u8, owner: [*:0]const u8) u32;
extern fn ra8_sci_init(channel: u8, cfg: *const SciCfg) u32;
extern fn ra8_sci_write_polling(channel: u8, data: [*]const u8, len: u32) u32;
extern fn ra8_sci_getc_polling(channel: u8, out_byte: *u8) u32;
extern fn ra8_sci_flush(channel: u8) u32;

const console = @import("console.zig");

/// `ra8_sci_cfg_t` (libs/ra8_hal/inc/ra8_sci.h).
const SciCfg = extern struct {
    baud: u32,
    data_bits: u8,
    parity: u8,
    stop_bits: u8,
    pclk_hz: u32,
};

pub fn pclkaHz(out_hz: *u32) u32 {
    return ra8_cgc_get_clock_hz(console.Wiring.clock_id_pclka, out_hz);
}

pub fn route(pin: u16, psel: u8, comptime owner: [:0]const u8) u32 {
    return ra8_pfs_route_peripheral(pin, psel, owner.ptr);
}

pub fn init(channel: u8, baud: u32, pclk_hz: u32) u32 {
    const cfg = SciCfg{
        .baud = baud,
        .data_bits = console.Framing.data_8,
        .parity = console.Framing.parity_none,
        .stop_bits = console.Framing.stop_1,
        .pclk_hz = pclk_hz,
    };
    return ra8_sci_init(channel, &cfg);
}

pub fn writePolling(channel: u8, data: []const u8) u32 {
    return ra8_sci_write_polling(channel, data.ptr, @intCast(data.len));
}

pub fn getc(channel: u8, out_byte: *u8) u32 {
    return ra8_sci_getc_polling(channel, out_byte);
}

pub fn flush(channel: u8) u32 {
    return ra8_sci_flush(channel);
}
