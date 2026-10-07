//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the D-PHY helpers in inc/ra8_mipi_phy_ops.h (RA8FW-575).
//! The PLL math, the timing table and the PCLKA setter stay in C. The
//! module-stop read goes through ra8_mstp_is_stopped so the NS alias
//! (RA8_PERIPH_NS_ALIAS) stays handled in one place.

const common = @import("abi_common.zig");
const ops = @import("internal/mipi_phy_ops.zig");

const tag = "MIPI_PHY";

var dual_mode: ops.Dual = .off;

extern fn priv_mipi_phy_compute_freq(pll: *const anyopaque, mosc_mhz: u8) u32;
extern fn priv_mipi_phy_find_timing(mode: u8, pclka_mhz: u8, rate_mbps: u16, out: *anyopaque) u16;
extern fn ra8_mipi_phy_set_pclka_freq(mhz: u8) u16;
extern fn ra8_mstp_is_stopped(id: u16, out_stopped: *bool) u16;

fn reg(off: usize) u32 {
    const p: *const volatile u32 = @ptrFromInt(ops.base_addr + off);
    return p.*;
}

/// Fails closed: an unreadable module-stop bit counts as stopped.
fn isStopped() bool {
    var stopped = true;
    if (ra8_mstp_is_stopped(ops.mstp_id, &stopped) != common.k_ra8_ok) return true;
    return stopped;
}

export fn ra8_mipi_phy_compute_pll_freq(pll: ?*const anyopaque, mosc_mhz: u8, out_mhz: ?*u32) u16 {
    const p = pll orelse {
        common.ra8_log_emit_error(tag, "pll must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    const out = out_mhz orelse {
        common.ra8_log_emit_error(tag, "out_mhz must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (!ops.moscOk(mosc_mhz)) return common.k_ra8_err_invalid_arg;
    out.* = priv_mipi_phy_compute_freq(p, mosc_mhz);
    return common.k_ra8_ok;
}

/// The C wrote through out_mbps unchecked; a null now returns null_ptr.
export fn ra8_mipi_phy_compute_lane_rate_mbps(pll: ?*const anyopaque, mosc_mhz: u8, out_mbps: ?*u32) u16 {
    var freq_mhz: u32 = 0;
    const err = ra8_mipi_phy_compute_pll_freq(pll, mosc_mhz, &freq_mhz);
    if (err != common.k_ra8_ok) return err;
    const out = out_mbps orelse return common.k_ra8_err_null_ptr;
    out.* = freq_mhz / ops.lane_rate_div;
    return common.k_ra8_ok;
}

export fn ra8_mipi_phy_lookup_timing(mode: u8, pclka_mhz: u8, rate_mbps: u16, out_timing: ?*anyopaque) u16 {
    const out = out_timing orelse {
        common.ra8_log_emit_error(tag, "out_timing must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    return priv_mipi_phy_find_timing(mode, pclka_mhz, rate_mbps, out);
}

export fn ra8_mipi_phy_get_status_decoded(out: ?*ops.Status) u16 {
    const o = out orelse {
        common.ra8_log_emit_error(tag, "out must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    o.* = ops.decodeStatus(reg(ops.off_sfr));
    return common.k_ra8_ok;
}

export fn ra8_mipi_phy_get_state() u8 {
    if (isStopped()) return @backingInt(ops.State.off);
    return @backingInt(ops.state(false, reg(ops.off_sfr), reg(ops.off_ocr)));
}

export fn ra8_mipi_phy_get_active_mode() u8 {
    if (isStopped()) return ops.mode_csi_device;
    return ops.activeMode(false, reg(ops.off_mdc));
}

export fn ra8_mipi_phy_set_dual_mode(mode: u8) u16 {
    dual_mode = ops.dualFromInt(mode) orelse return common.k_ra8_err_invalid_arg;
    return common.k_ra8_ok;
}

export fn ra8_mipi_phy_get_dual_mode() u8 {
    return @backingInt(dual_mode);
}

export fn ra8_mipi_phy_dual_mode_can_acquire(requestor: u8) bool {
    return ops.canAcquire(dual_mode, requestor);
}

export fn ra8_mipi_phy_set_pclka_freq_hz(hz: u32) u16 {
    const mhz = ops.pclkaMhz(hz) orelse return common.k_ra8_err_invalid_arg;
    return ra8_mipi_phy_set_pclka_freq(mhz);
}
