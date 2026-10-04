//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The seam under the board GPT profile: the two chip GPT adapters, the
//! facades that bind a handle, and the pin router. Declared apart from
//! `hal.zig` so a host suite fakes exactly these and nothing else.

const gpt = @import("gpt_types.zig");

const ErrCode = gpt.ErrCode;

pub extern fn fw_timer_ra8_iface() *const gpt.TimerIface;
pub extern fn fw_pwm_ra8_iface() *const gpt.PwmIface;
pub extern fn fw_timer_bind(tmr: *gpt.FwTimer, iface: *const gpt.TimerIface, ctx: ?*anyopaque) ErrCode;
pub extern fn fw_pwm_bind(pwm: *gpt.FwPwm, iface: *const gpt.PwmIface, ctx: ?*anyopaque) ErrCode;

/// `psel` is `ra8_psel_t`, an `enum : uint8_t`; `pin` is `ra8_port_pin_t`.
pub extern fn ra8_pfs_route_peripheral(pin: u16, psel: u8, owner: [*:0]const u8) ErrCode;
pub extern fn ra8_pin_validator_release(pin: u16) ErrCode;
