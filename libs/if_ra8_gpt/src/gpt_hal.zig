//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The slice of `libs/ra8_hal/inc/ra8_gpt.h` the adapters call. The HAL is
//! still C, so these are externs resolved at the final link, and the two
//! config structs are `extern struct` copies of the header's layout.

/// `ra8_gpt_mode_t`.
pub const Mode = struct {
    pub const saw_pwm: u8 = 0;
    pub const saw_one_shot: u8 = 1;
};

/// `ra8_gpt_prescaler_t`.
pub const Prescaler = struct {
    pub const div_1: u8 = 0;
};

/// `ra8_gpt_pwm_pin_t` and `ra8_gpt_ccr_sel_t`; only the A path is used.
pub const Pin = struct {
    pub const a: u8 = 0;
};
pub const Ccr = struct {
    pub const a: u8 = 0;
};

/// `ra8_gpt_pwm_polarity_t`, `ra8_gpt_pwm_stop_level_t`, `ra8_gpt_pwm_disable_t`.
pub const Polarity = struct {
    pub const active_high: u8 = 0;
    pub const active_low: u8 = 1;
};
pub const StopLevel = struct {
    pub const low: u8 = 0;
    pub const high: u8 = 1;
};
pub const Disable = struct {
    pub const none: u8 = 0;
};

/// `ra8_gpt_status_mask_t`: GTST.TCFA (compare/capture A) and GTST.TCFPO,
/// the overflow (wrap) flag.
pub const Status = struct {
    pub const ccra: u32 = 0x01;
    pub const overflow: u32 = 0x40;
};

/// `ra8_gpt_capture_src_t` (`ra8_gpt_capture.h`): no source, and every legal bit.
pub const CapSrc = struct {
    pub const none: u32 = 0;
    pub const valid_mask: u32 = 0x01FF_FFFF;
};

/// `ra8_gpt_cfg_t`.
pub const Cfg = extern struct {
    mode: u8,
    prescaler: u8,
    period: u32,
    duty_a: u32,
    duty_b: u32,
    auto_start: bool,
};

/// `ra8_gpt_pwm_pin_cfg_t`.
pub const PinCfg = extern struct {
    output_enable: bool,
    polarity: u8,
    stop_level: u8,
    disable_on_fault: u8,
};

pub extern fn ra8_gpt_init(channel: u8, cfg: *const Cfg) callconv(.c) u16;
pub extern fn ra8_gpt_deinit(channel: u8) callconv(.c) u16;
pub extern fn ra8_gpt_start(channel: u8) callconv(.c) u16;
pub extern fn ra8_gpt_stop(channel: u8) callconv(.c) u16;
pub extern fn ra8_gpt_read(channel: u8, out: ?*u32) callconv(.c) u16;
pub extern fn ra8_gpt_period_set(channel: u8, period_counts: u32) callconv(.c) u16;
pub extern fn ra8_gpt_set_duty(channel: u8, which: u8, value: u32) callconv(.c) u16;
pub extern fn ra8_gpt_duty_cycle_set(channel: u8, pin: u8, compare_counts: u32) callconv(.c) u16;
pub extern fn ra8_gpt_pwm_pin_configure(channel: u8, pin: u8, cfg: *const PinCfg) callconv(.c) u16;
pub extern fn ra8_gpt_get_status(channel: u8, out_mask: *u32) callconv(.c) u16;
pub extern fn ra8_gpt_clear_status(channel: u8, mask: u32) callconv(.c) u16;
pub extern fn ra8_gpt_capture_configure(channel: u8, which: u8, source_mask: u32) callconv(.c) u16;
pub extern fn ra8_gpt_capture_read(channel: u8, which: u8, out_value: ?*u32) callconv(.c) u16;

/// The saw-wave config both adapters open with: PCLKD/1, stopped, compares 0.
pub fn sawCfg(mode: u8, period: u32) Cfg {
    return .{ .mode = mode, .prescaler = Prescaler.div_1, .period = period, .duty_a = 0, .duty_b = 0, .auto_start = false };
}
