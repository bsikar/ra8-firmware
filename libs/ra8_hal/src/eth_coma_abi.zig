//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_eth_coma_* (internal/eth_coma.zig, RA8FW-550).
//! Built as its own object in libra8_hal.a (RA8FW-542) so an image links
//! only the units it calls.

const common = @import("abi_common.zig");
const coma = @import("internal/eth_coma.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_hw_timeout = common.k_ra8_err_hw_timeout;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_info = common.ra8_log_emit_info;
const ra8_log_emit_error = common.ra8_log_emit_error;
const ra8_log_emit_error_val = common.ra8_log_emit_error_val;

/// `ra8_mstp_t` k_ra8_mstp_eswm: (k_ra8_mstp_reg_c << 8) | 30 (inc/ra8_mstp_regs.h).
const mstp_eswm: u16 = (2 << 8) | 30;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

/// `ra8_eth_coma_event_fn_t`.
const EventFn = *const fn (ctx: ?*anyopaque, status_mask: u32) callconv(.C) void;

const tag = "ETHCMA";
const window: coma.Window = .{};

var handler_fn: ?EventFn = null;
var handler_ctx: ?*anyopaque = null;

/// `ra8_err_t ra8_eth_coma_init(void)`.
export fn ra8_eth_coma_init() u16 {
    const err = ra8_mstp_enable(mstp_eswm);
    if (err != k_ra8_ok) {
        ra8_log_emit_error(tag, "coma_init: mstp enable");
        ra8_log_emit_error_val(tag, "Error", err);
        return err;
    }
    coma.reset(window);
    ra8_log_emit_info(tag, "coma_init");
    return k_ra8_ok;
}

/// `ra8_err_t ra8_eth_coma_deinit(void)`.
export fn ra8_eth_coma_deinit() u16 {
    coma.quiesce(window);
    handler_fn = null;
    handler_ctx = null;
    return ra8_mstp_disable(mstp_eswm);
}

/// `ra8_err_t ra8_eth_coma_bringup(void)`.
export fn ra8_eth_coma_bringup() u16 {
    coma.bringup(window) catch {
        ra8_log_emit_error(tag, "coma_bringup: CABPIRM.BPR timeout");
        return k_ra8_err_hw_timeout;
    };
    ra8_log_emit_info(tag, "coma_bringup");
    return k_ra8_ok;
}

/// `ra8_err_t ra8_eth_coma_get_status(uint32_t* out_mask)`.
export fn ra8_eth_coma_get_status(out_mask: ?*u32) u16 {
    const out = out_mask orelse {
        ra8_log_emit_error(tag, "out_mask must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    out.* = coma.status(window);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_eth_coma_clear_status(uint32_t mask)`.
export fn ra8_eth_coma_clear_status(mask: u32) u16 {
    coma.clearStatus(window, mask);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_eth_coma_attach_handler(ra8_eth_coma_event_fn_t fn, void* ctx)`.
export fn ra8_eth_coma_attach_handler(func: ?EventFn, ctx: ?*anyopaque) u16 {
    handler_fn = func;
    handler_ctx = ctx;
    return k_ra8_ok;
}

/// `void ra8_eth_coma_dispatch(void)`: ISR-safe; latches the handler before
/// clearing so a concurrent attach sees a consistent pair.
export fn ra8_eth_coma_dispatch() void {
    const func = handler_fn;
    const ctx = handler_ctx;
    const mask = coma.takeStatus(window);
    if (func) |f| f(ctx, mask);
}

/// `ra8_err_t ra8_eth_coma_enter_stop(void)`.
export fn ra8_eth_coma_enter_stop() u16 {
    window.reg(coma.off_ctrl).* = 0;
    return ra8_mstp_disable(mstp_eswm);
}

/// `ra8_err_t ra8_eth_coma_exit_stop(void)`.
export fn ra8_eth_coma_exit_stop() u16 {
    return ra8_mstp_enable(mstp_eswm);
}
