//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_elc_* (internal/elc.zig, RA8FW-547). Built as its
//! own object in libra8_hal.a (RA8FW-542) so an image links only the units
//! it calls.

const common = @import("abi_common.zig");
const elc = @import("internal/elc.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_out_of_range = common.k_ra8_err_out_of_range;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_info = common.ra8_log_emit_info;
const ra8_log_emit_error = common.ra8_log_emit_error;
const ra8_log_emit_error_val = common.ra8_log_emit_error_val;

/// `ra8_mstp_t` k_ra8_mstp_elc: (k_ra8_mstp_reg_c << 8) | 14 (inc/ra8_mstp_regs.h).
const mstp_elc: u16 = (2 << 8) | 14;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

const tag = "ELC";
const window: elc.Window = .{ .base = elc.base };

/// `ra8_err_t ra8_elc_init(void)`.
export fn ra8_elc_init() u16 {
    ra8_log_emit_info(tag, "ra8_elc_init");
    const err = ra8_mstp_enable(mstp_elc);
    if (err != k_ra8_ok) {
        ra8_log_emit_error(tag, "elc_init: mstp enable");
        ra8_log_emit_error_val(tag, "Error", err);
        return err;
    }
    elc.clearRoutes(window);
    elc.setEnabled(window, true);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_elc_deinit(void)`.
export fn ra8_elc_deinit() u16 {
    elc.setEnabled(window, false);
    return ra8_mstp_disable(mstp_elc);
}

/// `ra8_err_t ra8_elc_link(uint8_t elsr_index, ra8_elc_event_t event)`.
export fn ra8_elc_link(elsr_index: u8, event: u16) u16 {
    elc.link(window, elsr_index, event) catch return k_ra8_err_out_of_range;
    return k_ra8_ok;
}

/// `ra8_err_t ra8_elc_unlink(uint8_t elsr_index)`.
export fn ra8_elc_unlink(elsr_index: u8) u16 {
    elc.unlink(window, elsr_index) catch return k_ra8_err_out_of_range;
    return k_ra8_ok;
}

/// `ra8_err_t ra8_elc_software_trigger(uint8_t event_index)`.
export fn ra8_elc_software_trigger(event_index: u8) u16 {
    elc.trigger(window, event_index) catch return k_ra8_err_invalid_arg;
    return k_ra8_ok;
}

/// `ra8_err_t ra8_elc_is_enabled(bool* out_enabled)`.
export fn ra8_elc_is_enabled(out_enabled: ?*bool) u16 {
    const out = out_enabled orelse {
        ra8_log_emit_error(tag, "is_enabled out");
        return k_ra8_err_null_ptr;
    };
    out.* = elc.isEnabled(window);
    return k_ra8_ok;
}
