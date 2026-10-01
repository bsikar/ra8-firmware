//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! XCLK for the OV5640: a GPT channel in saw PWM at 50% duty, routed out on
//! P501. The sensor needs its input clock running before it will answer on
//! SCCB at all, so this is the first thing the camera path does.

const hal = @import("hal.zig");
const pins = @import("camera_pins.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;

pub const Gpt = struct {
    /// GPT channel driving XCLK.
    pub const channel: u8 = 12;
    /// GTPR is 16-bit on this path, so the divider cannot exceed it.
    pub const period_max: u32 = 0xFFFF;
    /// Smallest divider that still yields a square wave.
    pub const period_min: u32 = 2;
};

/// Start XCLK at `frequency_hz`, derived from PCLKD.
///
/// The period is a whole divider of PCLKD, so an unrepresentable frequency is
/// refused rather than silently rounded: the sensor's PLL is configured for
/// the frequency the caller asked for.
pub fn start(frequency_hz: u32) u32 {
    if (frequency_hz == 0) return Err.invalid_arg;

    var pclkd_hz: u32 = 0;
    const clock_err = hal.ra8_cgc_get_clock_hz(vocab.ClockId.pclkd, &pclkd_hz);
    if (clock_err != Err.ok) return clock_err;

    const period = pclkd_hz / frequency_hz;
    if (period < Gpt.period_min or period > Gpt.period_max) return Err.invalid_arg;

    const timer_cfg = hal.GptCfg{
        .mode = vocab.Gpt.mode_saw_pwm,
        .prescaler = vocab.Gpt.ps_div_1,
        .period = period - 1,
        .duty_a = period / 2,
        .duty_b = 0,
        .auto_start = true,
    };
    const init_err = hal.ra8_gpt_init(Gpt.channel, &timer_cfg);
    if (init_err != Err.ok) return init_err;

    const pin_cfg = hal.GptPwmPinCfg{
        .output_enable = true,
        .polarity = vocab.Gpt.pol_active_high,
        .stop_level = vocab.Gpt.stop_low,
        .disable_on_fault = vocab.Gpt.disable_none,
    };
    const pin_err = hal.ra8_gpt_pwm_pin_configure(Gpt.channel, vocab.Gpt.pin_a, &pin_cfg);
    if (pin_err != Err.ok) return pin_err;

    const route_err = hal.ra8_pfs_route_peripheral(pins.xclk, vocab.Psel.gpt0, pins.xclk_owner);
    if (route_err != Err.ok) return route_err;

    return hal.ra8_pfs_set_drive_strength(pins.xclk, vocab.Dscr.high_speed_high);
}
