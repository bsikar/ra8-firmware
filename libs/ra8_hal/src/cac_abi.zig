//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the Clock Frequency Accuracy Measurement Circuit
//! (internal/cac.zig, RA8FW-568). Built as its own object in
//! libra8_hal.a (RA8FW-542).

const common = @import("abi_common.zig");
const cac = @import("internal/cac.zig");

const tag = "CAC";
const block = cac.Block{};

/// `k_ra8_mstp_cac`: (k_ra8_mstp_reg_c << 8) | 0, MSTPC0 (inc/ra8_mstp_regs.h).
const mstp_cac: u16 = 2 << 8;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

/// `ra8_cac_event_fn_t`.
const EventFn = *const fn (ctx: ?*anyopaque, status_mask: u8) callconv(.c) void;

var handler: ?EventFn = null;
var handler_ctx: ?*anyopaque = null;

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

/// `ra8_err_t ra8_cac_init(uint16_t upper, uint16_t lower)`.
export fn ra8_cac_init(upper: u16, lower: u16) u16 {
    const err = ra8_mstp_enable(mstp_cac);
    if (err != common.k_ra8_ok) {
        common.ra8_log_emit_error(tag, "cac_init: mstp enable");
        common.ra8_log_emit_error_val(tag, "Error", err);
        return err;
    }
    block.configure(upper, lower);
    common.ra8_log_emit_info(tag, "cac_init");
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_cac_measure(uint16_t* out_count)`.
export fn ra8_cac_measure(out_count: ?*u16) u16 {
    const out = out_count orelse return nullPtr("out_count must not be nullptr");
    out.* = block.measure() catch return common.k_ra8_err_hw_timeout;
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_cac_deinit(void)`.
export fn ra8_cac_deinit() u16 {
    block.shutdown();
    handler = null;
    handler_ctx = null;
    return ra8_mstp_disable(mstp_cac);
}

/// `ra8_err_t ra8_cac_get_status(uint8_t* out_mask)`.
export fn ra8_cac_get_status(out_mask: ?*u8) u16 {
    const out = out_mask orelse return nullPtr("out_mask must not be nullptr");
    out.* = block.status();
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_cac_clear_status(uint8_t mask)`.
export fn ra8_cac_clear_status(mask: u8) u16 {
    block.clear(mask);
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_cac_attach_handler(ra8_cac_event_fn_t fn, void* ctx)`.
export fn ra8_cac_attach_handler(func: ?EventFn, ctx: ?*anyopaque) u16 {
    handler = func;
    handler_ctx = ctx;
    return common.k_ra8_ok;
}

/// `void ra8_cac_dispatch(void)`: acknowledge, then call the handler.
export fn ra8_cac_dispatch() void {
    const mask = block.takePending();
    if (handler) |f| f(handler_ctx, mask);
}

/// `ra8_err_t ra8_cac_enter_stop(void)`.
export fn ra8_cac_enter_stop() u16 {
    block.stop();
    return ra8_mstp_disable(mstp_cac);
}

/// `ra8_err_t ra8_cac_exit_stop(void)`.
export fn ra8_cac_exit_stop() u16 {
    return ra8_mstp_enable(mstp_cac);
}
