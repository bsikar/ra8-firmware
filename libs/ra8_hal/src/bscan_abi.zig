//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_bscan_* (internal/bscan.zig, RA8FW-551). Built as
//! its own object in libra8_hal.a (RA8FW-542) so an image links only the
//! units it calls.

const common = @import("abi_common.zig");
const bscan = @import("internal/bscan.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_not_initialized = common.k_ra8_err_not_initialized;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_info = common.ra8_log_emit_info;
const ra8_log_emit_info_val = common.ra8_log_emit_info_val;
const ra8_log_emit_error = common.ra8_log_emit_error;

const tag = "BSCAN";
var state: bscan.State = .{};

fn code(err: bscan.Error) u16 {
    return switch (err) {
        error.NotInitialized => k_ra8_err_not_initialized,
        error.InvalidArg => k_ra8_err_invalid_arg,
    };
}

/// `ra8_err_t ra8_bscan_init(void)`.
export fn ra8_bscan_init() u16 {
    state.init();
    ra8_log_emit_info(tag, "bscan init");
    return k_ra8_ok;
}

/// `ra8_err_t ra8_bscan_deinit(void)`.
export fn ra8_bscan_deinit() u16 {
    state.deinit();
    return k_ra8_ok;
}

/// `ra8_err_t ra8_bscan_get_idcode(uint32_t* out)`.
export fn ra8_bscan_get_idcode(out: ?*u32) u16 {
    const dst = out orelse {
        ra8_log_emit_error(tag, "out must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    dst.* = state.idcode() catch |err| return code(err);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_bscan_get_status(ra8_bscan_status_t* out)`.
export fn ra8_bscan_get_status(out: ?*bscan.Status) u16 {
    const dst = out orelse {
        ra8_log_emit_error(tag, "out must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    dst.* = state.status();
    return k_ra8_ok;
}

/// `ra8_err_t ra8_bscan_clear_status(uint32_t mask)`.
export fn ra8_bscan_clear_status(mask: u32) u16 {
    state.clear(mask) catch |err| return code(err);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_bscan_set_instruction(ra8_bscan_instr_t instr)`.
export fn ra8_bscan_set_instruction(instr: u8) u16 {
    state.setInstruction(instr) catch |err| return code(err);
    ra8_log_emit_info_val(tag, "set instruction", instr);
    return k_ra8_ok;
}
