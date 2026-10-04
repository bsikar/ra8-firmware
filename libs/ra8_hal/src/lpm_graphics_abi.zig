//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI export for the graphics power domain bring-up
//! (internal/lpm_graphics.zig, RA8FW-556). Built as its own object in
//! libra8_hal.a (RA8FW-542). Log lines match the deleted
//! ra8_lpm_graphics.c, including RA8_RETURN_ON_ERROR's message-then-
//! "Error" pairs.

const common = @import("abi_common.zig");
const gfx = @import("internal/lpm_graphics.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_hw_timeout = common.k_ra8_err_hw_timeout;

const tag = "LPM";

/// `RA8_RETURN_ON_ERROR` with hw_timeout.
fn logReturn(msg: [*:0]const u8) void {
    common.ra8_log_emit_error(tag, msg);
    common.ra8_log_emit_error_val(tag, "Error", k_ra8_err_hw_timeout);
}

fn logFailure(err: gfx.Error) u16 {
    switch (err) {
        error.ZeroTimeout => {
            common.ra8_log_emit_error(tag, "graphics_power_on: timeout_iters == 0");
            return k_ra8_err_invalid_arg;
        },
        error.BusyBeforeOn => {
            logReturn("graphics_power_on: PDCSF busy");
            logReturn("graphics_power_on: not ready");
        },
        error.NotReady => logReturn("graphics_power_on: not ready"),
        error.StuckAfterOn => {
            logReturn("graphics_power_on: PDCSF stuck");
            logReturn("graphics_power_on: still gated");
        },
        error.StillGated => logReturn("graphics_power_on: still gated"),
    }
    return k_ra8_err_hw_timeout;
}

/// `ra8_err_t ra8_lpm_graphics_power_on(uint32_t timeout_iters)`.
export fn ra8_lpm_graphics_power_on(timeout_iters: u32) u16 {
    const outcome = gfx.powerOn(.{ .base = gfx.base }, timeout_iters) catch |err| return logFailure(err);
    if (outcome == .powered_on) common.ra8_log_emit_info(tag, "graphics power domain on");
    return k_ra8_ok;
}
