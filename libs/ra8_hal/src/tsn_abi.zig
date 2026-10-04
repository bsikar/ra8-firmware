//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for inc/ra8_tsn.h (RA8FW-577). Module stop and the ADC
//! temperature channel stay in C behind externs.

const common = @import("abi_common.zig");
const tsn = @import("internal/tsn.zig");

const tag = "TSN";

var initialized: bool = false;
var high_ref: i16 = tsn.temp_high_125;
var low_ref: i16 = tsn.temp_low_n40;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn ra8_adc_read_internal_channel(chan: u8, out_raw: *u16) u16;

fn tscr() *volatile u8 {
    return @ptrFromInt(tsn.ctrl_base);
}

fn calWord(index: usize) u32 {
    const p: *const volatile u32 = @ptrFromInt(tsn.cal_base + 4 * index);
    return p.*;
}

fn fail(err: u16, message: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, message);
    common.ra8_log_emit_error_val(tag, "Error", err);
    return err;
}

fn busyWaitUs(usec: u16) void {
    var u: u16 = 0;
    while (u < usec) : (u += 1) {
        var i: u16 = 0;
        while (i < tsn.busy_loops_per_us) : (i += 1) asm volatile ("nop");
    }
}

export fn ra8_tsn_init(cfg: ?*const tsn.Config) u16 {
    const c = cfg orelse {
        common.ra8_log_emit_error(tag, "cfg must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (!tsn.configOk(c.*)) return fail(common.k_ra8_err_invalid_arg, "tsn_init: cfg invalid");
    const err = ra8_mstp_enable(tsn.mstp_id);
    if (err != common.k_ra8_ok) return fail(err, "tsn_init: mstp enable");
    tscr().* = tsn.tscr_tsen;
    busyWaitUs(c.stab_us);
    tscr().* = tsn.tscr_tsen | tsn.tscr_tsoe;
    high_ref = c.high_ref_degc;
    low_ref = c.low_ref_degc;
    initialized = true;
    common.ra8_log_emit_info(tag, "tsn_init");
    return common.k_ra8_ok;
}

/// TSOE first, then TSEN (HUM 55.3.2); the flag drops unconditionally.
export fn ra8_tsn_deinit() u16 {
    tscr().* = tsn.tscr_tsen;
    tscr().* = 0;
    initialized = false;
    _ = ra8_mstp_disable(tsn.mstp_id);
    return common.k_ra8_ok;
}

export fn ra8_tsn_read_raw(raw: u16, out_code: ?*u16) u16 {
    const out = out_code orelse {
        common.ra8_log_emit_error(tag, "out_code must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (!initialized) return common.k_ra8_err_invalid_state;
    out.* = tsn.maskCode(raw);
    return common.k_ra8_ok;
}

export fn ra8_tsn_convert_to_milli_c(raw_code: u16, out_milli_c: ?*i32) u16 {
    const out = out_milli_c orelse {
        common.ra8_log_emit_error(tag, "out_milli_c must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (!initialized) return common.k_ra8_err_invalid_state;
    out.* = tsn.convert(raw_code, calWord(0), calWord(1), high_ref, low_ref) orelse
        return common.k_ra8_err_invalid_state;
    return common.k_ra8_ok;
}

export fn ra8_tsn_read_die_temp_milli_c(out_milli_c: ?*i32) u16 {
    if (out_milli_c == null) {
        common.ra8_log_emit_error(tag, "out_milli_c must not be nullptr");
        return common.k_ra8_err_null_ptr;
    }
    if (!initialized) return common.k_ra8_err_invalid_state;
    var adc_raw: u16 = 0;
    const adc_err = ra8_adc_read_internal_channel(tsn.adc_chan_temperature, &adc_raw);
    if (adc_err != common.k_ra8_ok) return fail(adc_err, "die_temp: adc read");
    var code: u16 = 0;
    const mask_err = ra8_tsn_read_raw(adc_raw, &code);
    if (mask_err != common.k_ra8_ok) return fail(mask_err, "die_temp: read_raw");
    return ra8_tsn_convert_to_milli_c(code, out_milli_c);
}

export fn ra8_tsn_get_status(out_tscr: ?*u8) u16 {
    const out = out_tscr orelse {
        common.ra8_log_emit_error(tag, "out_tscr must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    out.* = tscr().* & tsn.tscr_all;
    return common.k_ra8_ok;
}

export fn ra8_tsn_clear_status() u16 {
    tscr().* = 0;
    return common.k_ra8_ok;
}

export fn ra8_tsn_enter_stop() u16 {
    return ra8_mstp_disable(tsn.mstp_id);
}

export fn ra8_tsn_exit_stop() u16 {
    return ra8_mstp_enable(tsn.mstp_id);
}
