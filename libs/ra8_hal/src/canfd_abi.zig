//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_canfd_set_tdc (internal/canfd_tdc.zig, RA8FW-532). Built as its own object in
//! libra8_hal.a (RA8FW-542) so an image links only the units it calls.

const common = @import("abi_common.zig");
const canfd_tdc = @import("internal/canfd_tdc.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_info = common.ra8_log_emit_info;
const ra8_log_emit_error = common.ra8_log_emit_error;

/// `ra8_chmdc_mode_t` (inc/ra8_canfd_regs.h).
const k_ra8_chmdc_operation: c_uint = 0;
const k_ra8_chmdc_reset: c_uint = 1;

extern fn priv_ra8_canfd_internal_set_channel_mode(reg: *volatile anyopaque, mode: c_uint) u16;

const canfd_tag = "CANFD";

/// `ra8_err_t ra8_canfd_set_tdc(uint8_t channel, const ra8_canfd_tdc_cfg_t* cfg)`
/// (inc/ra8_canfd.h): read-modify-write CFDCnFDCFG with the channel in
/// CH_RESET, then back to operation. HUM Ch 41 "CFDCnFDCFG" p 2788.
export fn ra8_canfd_set_tdc(channel: u8, cfg: ?*const canfd_tdc.Cfg) u16 {
    const base = canfd_tdc.validate(channel, cfg) catch |err| switch (err) {
        error.NullCfg => {
            ra8_log_emit_error(canfd_tag, "cfg must not be nullptr");
            return k_ra8_err_null_ptr;
        },
        error.ChannelOutOfRange => {
            ra8_log_emit_error(canfd_tag, "channel out of range");
            return k_ra8_err_null_ptr;
        },
        error.OffsetTooLarge => return k_ra8_err_invalid_arg,
    };
    const config = cfg.?;
    const reg: *volatile anyopaque = @ptrFromInt(base);
    const reset_err = priv_ra8_canfd_internal_set_channel_mode(reg, k_ra8_chmdc_reset);
    if (reset_err != k_ra8_ok) return reset_err;
    const fdcfg: *volatile u32 = @ptrFromInt(base + canfd_tdc.off_fdcfg);
    fdcfg.* = canfd_tdc.fdcfgValue(fdcfg.*, config.*);
    const op_err = priv_ra8_canfd_internal_set_channel_mode(reg, k_ra8_chmdc_operation);
    if (op_err != k_ra8_ok) return op_err;
    ra8_log_emit_info(canfd_tag, "tdc configured");
    return k_ra8_ok;
}
