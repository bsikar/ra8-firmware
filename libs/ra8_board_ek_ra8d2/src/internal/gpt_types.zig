//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Layout mirrors of the `fw_if_timer` and `fw_if_pwm` vocabulary the board
//! GPT profile implements. Kept apart from the profile, as `clock_types.zig`
//! is, so the seam declarations and the profile can both name them.

/// `ra8_err_t` as a C return: `enum : uint16_t`, read as 16 bits so no
/// undefined upper half reaches a comparison (see `hal.ErrCode`).
pub const ErrCode = u16;

/// `fw_timer_ch_t`: a board timer index.
pub const TimerCh = extern struct { index: u8 };

/// `fw_timer_caps_t`.
pub const TimerCaps = extern struct {
    channel_count: u8 = 0,
    counter_bits: u8 = 0,
    counter_max: u32 = 0,
    has_capture: bool = false,
    has_one_shot: bool = false,
};

/// `fw_timer_iface_t`. `mode` is `fw_timer_mode_t`, an `enum : uint8_t`.
pub const TimerIface = extern struct {
    get_caps: *const fn (?*anyopaque, *TimerCaps) callconv(.c) ErrCode,
    open: *const fn (?*anyopaque, TimerCh, u8, u32) callconv(.c) ErrCode,
    close: *const fn (?*anyopaque, TimerCh) callconv(.c) ErrCode,
    start: *const fn (?*anyopaque, TimerCh) callconv(.c) ErrCode,
    stop: *const fn (?*anyopaque, TimerCh) callconv(.c) ErrCode,
    read: *const fn (?*anyopaque, TimerCh, *u32) callconv(.c) ErrCode,
    set_period: *const fn (?*anyopaque, TimerCh, u32) callconv(.c) ErrCode,
    capture_read: *const fn (?*anyopaque, TimerCh, *u32) callconv(.c) ErrCode,
    take_wrap: *const fn (?*anyopaque, TimerCh, *bool) callconv(.c) ErrCode,
};

/// `fw_timer_t`: the caller-allocated handle `fw_timer_bind` fills.
pub const FwTimer = extern struct {
    iface: ?*const TimerIface = null,
    ctx: ?*anyopaque = null,
    caps: TimerCaps = .{},
    bound: bool = false,
};

/// `fw_pwm_ch_t`: a board PWM output index.
pub const PwmCh = extern struct { index: u8 };

/// `fw_pwm_caps_t`.
pub const PwmCaps = extern struct {
    channel_count: u8 = 0,
    counter_bits: u8 = 0,
    period_max: u32 = 0,
    has_active_low: bool = false,
};

/// `fw_pwm_iface_t`. `pol` is `fw_pwm_polarity_t`, an `enum : uint8_t`.
pub const PwmIface = extern struct {
    get_caps: *const fn (?*anyopaque, *PwmCaps) callconv(.c) ErrCode,
    open: *const fn (?*anyopaque, PwmCh, u32, u8) callconv(.c) ErrCode,
    close: *const fn (?*anyopaque, PwmCh) callconv(.c) ErrCode,
    start: *const fn (?*anyopaque, PwmCh) callconv(.c) ErrCode,
    stop: *const fn (?*anyopaque, PwmCh) callconv(.c) ErrCode,
    set_period: *const fn (?*anyopaque, PwmCh, u32) callconv(.c) ErrCode,
    set_duty: *const fn (?*anyopaque, PwmCh, u32) callconv(.c) ErrCode,
};

/// `fw_pwm_t`: the caller-allocated handle `fw_pwm_bind` fills.
pub const FwPwm = extern struct {
    iface: ?*const PwmIface = null,
    ctx: ?*anyopaque = null,
    caps: PwmCaps = .{},
    bound: bool = false,
};
