//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The standard board prologue, in the order every application needs it:
//! clock tree, module-stop table, live rates, timebase, then the optional
//! console and LEDs, with interrupts unmasked last so nothing dispatches into
//! a half-initialised driver.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// LED bits a caller may name.
pub const Leds = struct {
    pub const none: u32 = 0x0;
    pub const led1: u32 = 0x1;
    pub const led2: u32 = 0x2;
    pub const led3: u32 = 0x4;
    pub const all: u32 = 0x7;
    pub const count: u32 = 3;
};

/// `ra8_board_bringup_cfg_t`.
pub const Cfg = extern struct {
    console_baud: u32,
    leds_mask: u32,
    enable_interrupts: bool,
};

/// `ra8_board_bringup_out_t`: the rates the timebase and console were sized on.
pub const Rates = extern struct {
    cpuclk0_hz: u32,
    pclka_hz: u32,
};

/// Bring up each named LED in id order, so a contested pin reports the same
/// LED whichever other bits the caller set.
fn initLeds(leds_mask: u32) u32 {
    var led: u32 = 0;
    while (led < Leds.count) : (led += 1) {
        if ((leds_mask & (@as(u32, 1) << @intCast(led))) == 0) continue;
        const err = hal.ra8_board_led_init(led);
        if (err != Err.ok) return err;
    }
    return Err.ok;
}

/// Steps 1 to 4: the half of the prologue every application needs.
///
/// Reads both rates back from the clock generator rather than restating the
/// board constants, so what the caller receives is what the timebase and the
/// console divisor were actually computed against.
fn substrate(out: *Rates) u32 {
    var board_rates: hal.ClockRates = .{ .cpuclk0_hz = 0, .pclka_hz = 0 };
    var err = hal.ra8_board_clocks_init(&board_rates);
    if (err != Err.ok) return err;

    err = hal.ra8_mstp_init();
    if (err != Err.ok) return err;

    var cpuclk0_hz: u32 = 0;
    err = hal.ra8_cgc_get_clock_hz(vocab.ClockId.cpuclk0, &cpuclk0_hz);
    if (err != Err.ok) return err;

    var pclka_hz: u32 = 0;
    err = hal.ra8_cgc_get_clock_hz(vocab.ClockId.pclka, &pclka_hz);
    if (err != Err.ok) return err;

    err = hal.ra8_time_init(cpuclk0_hz);
    if (err != Err.ok) return err;

    out.* = .{ .cpuclk0_hz = cpuclk0_hz, .pclka_hz = pclka_hz };
    return Err.ok;
}

/// Run the whole prologue. @p out is written only on success.
pub fn run(cfg: ?*const Cfg, out: ?*Rates) u32 {
    const want = cfg orelse return Err.null_ptr;
    const dst = out orelse return Err.null_ptr;
    if ((want.leds_mask & ~Leds.all) != 0) return Err.invalid_arg;

    var rates: Rates = .{ .cpuclk0_hz = 0, .pclka_hz = 0 };
    var err = substrate(&rates);
    if (err != Err.ok) return err;

    if (want.console_baud != 0) {
        err = hal.ra8_board_uart_console_init(want.console_baud);
        if (err != Err.ok) return err;
    }

    err = initLeds(want.leds_mask);
    if (err != Err.ok) return err;

    if (want.enable_interrupts) hal.ra8_isr_globals_enable();

    dst.* = rates;
    return Err.ok;
}
