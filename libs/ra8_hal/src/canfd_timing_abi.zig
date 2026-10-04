//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_canfd_set_bitrate / ra8_canfd_set_brs (RA8FW-580).
//! Channel-mode changes and the PCLKA query stay in C behind externs.

const common = @import("abi_common.zig");
const timing = @import("internal/canfd_timing.zig");

const tag = "CANFD";

/// `ra8_chmdc_mode_t` (inc/ra8_canfd_regs.h).
const chmdc_operation: c_uint = 0;
const chmdc_reset: c_uint = 1;
/// `k_ra8_clock_id_pclka` (inc/ra8_cgc.h).
const clock_id_pclka: u8 = 3;

extern fn priv_ra8_canfd_internal_set_channel_mode(reg: *volatile anyopaque, mode: c_uint) u16;
extern fn ra8_cgc_get_clock_hz(id: u8, out_hz: ?*u32) u16;

fn word(base: usize, offset: usize) *volatile u32 {
    return @ptrFromInt(base + offset);
}

fn setMode(base: usize, mode: c_uint) u16 {
    return priv_ra8_canfd_internal_set_channel_mode(@ptrFromInt(base), mode);
}

fn channelBase(channel: u8) ?usize {
    return timing.channelBase(channel) orelse {
        common.ra8_log_emit_error(tag, "channel out of range");
        return null;
    };
}

fn pclka(out_hz: *u32) u16 {
    return ra8_cgc_get_clock_hz(clock_id_pclka, out_hz);
}

export fn ra8_canfd_set_bitrate(channel: u8, bitrate_bps: u32, data_bitrate_bps: u32) u16 {
    const base = channelBase(channel) orelse return common.k_ra8_err_null_ptr;
    var hz: u32 = 0;
    const clk_err = pclka(&hz);
    if (clk_err != common.k_ra8_ok) return clk_err;
    const nominal = timing.solve(hz, bitrate_bps, timing.prescaler_max) orelse
        return common.k_ra8_err_invalid_arg;
    const halt_err = setMode(base, chmdc_reset);
    if (halt_err != common.k_ra8_ok) return halt_err;
    word(base, timing.off_ncfg).* = timing.packNcfg(nominal);
    if (data_bitrate_bps != 0 and data_bitrate_bps > bitrate_bps) {
        const data = timing.solve(hz, data_bitrate_bps, timing.data_prescaler_max) orelse {
            _ = setMode(base, chmdc_operation);
            return common.k_ra8_err_invalid_arg;
        };
        word(base, timing.off_dcfg).* = timing.packDcfg(data);
    }
    const op_err = setMode(base, chmdc_operation);
    if (op_err != common.k_ra8_ok) return op_err;
    common.ra8_log_emit_info_val(tag, "set_bitrate bps", bitrate_bps);
    return common.k_ra8_ok;
}

export fn ra8_canfd_set_brs(channel: u8, fast_bitrate: u32) u16 {
    const base = channelBase(channel) orelse return common.k_ra8_err_null_ptr;
    if (fast_bitrate == 0) return common.k_ra8_err_invalid_arg;
    var hz: u32 = 0;
    const clk_err = pclka(&hz);
    if (clk_err != common.k_ra8_ok) return clk_err;
    const data = timing.solve(hz, fast_bitrate, timing.data_prescaler_max) orelse
        return common.k_ra8_err_invalid_arg;
    word(base, timing.off_dcfg).* = timing.packDcfg(data);
    return common.k_ra8_ok;
}
