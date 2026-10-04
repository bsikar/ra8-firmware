//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the power-management glue (internal/pwr.zig,
//! RA8FW-570). Built as its own object in libra8_hal.a (RA8FW-542).

const common = @import("abi_common.zig");
const pwr = @import("internal/pwr.zig");

const tag = "PWR";
const block = pwr.Block{};

extern fn ra8_mstp_init() u16;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn ra8_cgc_get_clock_hz(id: u8, out_hz: ?*u32) u16;

/// `ra8_err_t ra8_pwr_init(void)`.
export fn ra8_pwr_init() u16 {
    common.ra8_log_emit_info(tag, "ra8_pwr_init");
    const err = ra8_mstp_init();
    if (err != common.k_ra8_ok) {
        common.ra8_log_emit_error_val(tag, "mstp init failed", err);
        return err;
    }
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_pwr_module_request(ra8_mstp_t id)`.
export fn ra8_pwr_module_request(id: u16) u16 {
    return ra8_mstp_enable(id);
}

/// `ra8_err_t ra8_pwr_module_release(ra8_mstp_t id)`.
export fn ra8_pwr_module_release(id: u16) u16 {
    return ra8_mstp_disable(id);
}

/// `ra8_err_t ra8_pwr_set_wake_source(ra8_pwr_wake_t source)`.
export fn ra8_pwr_set_wake_source(source: u16) u16 {
    const w = pwr.Wake.decode(source) orelse {
        common.ra8_log_emit_error_val(tag, "set_wake: invalid source", source);
        return common.k_ra8_err_invalid_arg;
    };
    block.enable(w);
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_pwr_clear_wake_source(ra8_pwr_wake_t source)`.
export fn ra8_pwr_clear_wake_source(source: u16) u16 {
    const w = pwr.Wake.decode(source) orelse {
        common.ra8_log_emit_error_val(tag, "clear_wake: invalid source", source);
        return common.k_ra8_err_invalid_arg;
    };
    block.disable(w);
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_pwr_wake_source_is_enabled(ra8_pwr_wake_t, bool*)`.
export fn ra8_pwr_wake_source_is_enabled(source: u16, out_enabled: ?*bool) u16 {
    const out = out_enabled orelse {
        common.ra8_log_emit_error(tag, "wake_source_is_enabled: out_enabled");
        return common.k_ra8_err_null_ptr;
    };
    const w = pwr.Wake.decode(source) orelse return common.k_ra8_err_invalid_arg;
    out.* = block.isEnabled(w);
    return common.k_ra8_ok;
}

/// `void ra8_pwr_enter_sleep(void)`.
export fn ra8_pwr_enter_sleep() void {
    pwr.waitForInterrupt();
}

/// `ra8_err_t ra8_pwr_enter_software_standby(void)`.
export fn ra8_pwr_enter_software_standby() u16 {
    if (!block.anyArmed()) {
        common.ra8_log_emit_error(tag, "software_standby with no wake source armed");
        return common.k_ra8_err_invalid_state;
    }
    pwr.waitForInterrupt();
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_pwr_get_clock_hz(ra8_clock_id_t id, uint32_t* out_hz)`.
export fn ra8_pwr_get_clock_hz(id: u8, out_hz: ?*u32) u16 {
    return ra8_cgc_get_clock_hz(id, out_hz);
}
