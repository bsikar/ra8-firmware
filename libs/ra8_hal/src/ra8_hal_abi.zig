//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports of the ported ra8_hal units. The prototypes in inc/ are
//! unchanged and stay the membrane: callers cannot tell which side of the
//! port a symbol is on.

const canfd_tdc = @import("internal/canfd_tdc.zig");
const eth_media = @import("internal/eth_media.zig");

extern fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// `ra8_err_t` values (libs/ra8_core/inc/ra8_err.h).
const k_ra8_ok: u16 = 0;
const k_ra8_err_invalid_arg: u16 = 0x103;
const k_ra8_err_null_ptr: u16 = 0x504;

const eth_tag = "ETH";

/// `ra8_err_t ra8_eth_rgmii_select(ra8_eth_mii_port_t port)` (inc/ra8_eth.h).
export fn ra8_eth_rgmii_select(port: u8) u16 {
    eth_media.rgmiiSelect(eth_media.hardware(), port) catch {
        ra8_log_emit_error(eth_tag, "Range check failed");
        return k_ra8_err_invalid_arg;
    };
    ra8_log_emit_info(eth_tag, "rgmii_select");
    return k_ra8_ok;
}

/// `ra8_chmdc_mode_t` (inc/ra8_canfd_regs.h).
const k_ra8_chmdc_operation: c_uint = 0;
const k_ra8_chmdc_reset: c_uint = 1;

extern fn priv_ra8_canfd_internal_set_channel_mode(reg: *volatile anyopaque, mode: c_uint) u16;

const canfd_tag = "CANFD";

/// `ra8_err_t ra8_canfd_set_tdc(uint8_t channel, const ra8_canfd_tdc_cfg_t* cfg)`
/// (inc/ra8_canfd.h): read-modify-write CFDCnFDCFG with the channel in
/// CH_RESET, then back to operation. HUM Ch 41 "CFDCnFDCFG" p 2788.
export fn ra8_canfd_set_tdc(channel: u8, cfg: ?*const canfd_tdc.Cfg) u16 {
    const config = cfg orelse {
        ra8_log_emit_error(canfd_tag, "cfg must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    const base = canfd_tdc.channelBase(channel) orelse {
        ra8_log_emit_error(canfd_tag, "channel out of range");
        return k_ra8_err_null_ptr;
    };
    if (config.offset > canfd_tdc.offset_max) return k_ra8_err_invalid_arg;
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
