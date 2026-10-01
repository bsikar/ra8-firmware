//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Layout mirrors of the `fw_if_clock` vocabulary the board clock profile
//! implements. Kept apart from the profile itself so the seam declarations and
//! the wiring table can both name them without importing each other.

/// `fw_clock_module_kind_t`.
pub const Kind = struct {
    pub const none: u8 = 0;
    pub const core: u8 = 1;
    pub const uart: u8 = 2;
    pub const spi: u8 = 3;
    pub const i2c: u8 = 4;
    pub const can: u8 = 5;
    pub const timer: u8 = 6;
    pub const pwm: u8 = 7;
    pub const adc: u8 = 8;
    pub const dac: u8 = 9;
    pub const dma: u8 = 10;
    pub const display: u8 = 11;
    pub const camera: u8 = 12;
    pub const usb: u8 = 13;
    pub const ethernet: u8 = 14;
    pub const sdhost: u8 = 15;
    pub const crypto: u8 = 16;
    pub const rtc: u8 = 17;
    pub const watchdog: u8 = 18;
    pub const memory: u8 = 19;
    pub const count: usize = 20;
};

/// `fw_clock_module_t`: which kind of block, and which instance of it.
pub const Module = extern struct {
    kind: u8,
    index: u8,
};

/// `fw_clock_iface_t`: the three ops a binding supplies.
pub const FwClockIface = extern struct {
    rate_for: *const fn (ctx: ?*anyopaque, module: Module, out_hz: *u32) callconv(.c) u32,
    set_gate: *const fn (ctx: ?*anyopaque, module: Module, on: bool) callconv(.c) u32,
    has_module: *const fn (ctx: ?*anyopaque, module: Module, out_present: *bool) callconv(.c) u32,
};

/// `fw_clock_t`: the caller-allocated handle `fw_clock_bind` fills.
pub const FwClock = extern struct {
    iface: ?*const FwClockIface = null,
    ctx: ?*anyopaque = null,
    bound: bool = false,
};
