//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_eth_mfwd_* (internal/eth_mfwd.zig, RA8FW-552).
//! Built as its own object in libra8_hal.a (RA8FW-542) so an image links
//! only the units it calls.

const common = @import("abi_common.zig");
const mfwd = @import("internal/eth_mfwd.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_info = common.ra8_log_emit_info;
const ra8_log_emit_error = common.ra8_log_emit_error;
const ra8_log_emit_error_val = common.ra8_log_emit_error_val;

/// `ra8_mstp_t` k_ra8_mstp_eswm: (k_ra8_mstp_reg_c << 8) | 30 (inc/ra8_mstp_regs.h).
const mstp_eswm: u16 = (2 << 8) | 30;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

/// `ra8_eth_mfwd_event_fn_t`.
const EventFn = *const fn (ctx: ?*anyopaque, status_mask: u32) callconv(.c) void;

const tag = "ETHMFW";
const window: mfwd.Window = .{};

var handler_fn: ?EventFn = null;
var handler_ctx: ?*anyopaque = null;

/// `ra8_err_t ra8_eth_mfwd_init(void)`.
export fn ra8_eth_mfwd_init() u16 {
    const err = ra8_mstp_enable(mstp_eswm);
    if (err != k_ra8_ok) {
        ra8_log_emit_error(tag, "mfwd_init: mstp enable");
        ra8_log_emit_error_val(tag, "Error", err);
        return err;
    }
    mfwd.reset(window);
    ra8_log_emit_info(tag, "mfwd_init");
    return k_ra8_ok;
}

/// `ra8_err_t ra8_eth_mfwd_deinit(void)`.
export fn ra8_eth_mfwd_deinit() u16 {
    mfwd.quiesce(window);
    handler_fn = null;
    handler_ctx = null;
    return ra8_mstp_disable(mstp_eswm);
}

/// `ra8_err_t ra8_eth_mfwd_get_status(uint32_t* out_mask)`.
export fn ra8_eth_mfwd_get_status(out_mask: ?*u32) u16 {
    const out = out_mask orelse {
        ra8_log_emit_error(tag, "out_mask must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    out.* = mfwd.status(window);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_eth_mfwd_clear_status(uint32_t mask)`.
export fn ra8_eth_mfwd_clear_status(mask: u32) u16 {
    mfwd.clearStatus(window, mask);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_eth_mfwd_attach_handler(ra8_eth_mfwd_event_fn_t fn, void* ctx)`.
export fn ra8_eth_mfwd_attach_handler(func: ?EventFn, ctx: ?*anyopaque) u16 {
    handler_fn = func;
    handler_ctx = ctx;
    return k_ra8_ok;
}

/// `void ra8_eth_mfwd_dispatch(void)`: ISR-safe; latches the handler, clears
/// STS through ICLR, then calls it with what was pending.
export fn ra8_eth_mfwd_dispatch() void {
    const func = handler_fn;
    const ctx = handler_ctx;
    const mask = mfwd.takeStatus(window);
    if (func) |f| f(ctx, mask);
}

/// `ra8_err_t ra8_eth_mfwd_enter_stop(void)`.
export fn ra8_eth_mfwd_enter_stop() u16 {
    window.reg(mfwd.off_ctrl).* = 0;
    return ra8_mstp_disable(mstp_eswm);
}

/// `ra8_err_t ra8_eth_mfwd_exit_stop(void)`.
export fn ra8_eth_mfwd_exit_stop() u16 {
    return ra8_mstp_enable(mstp_eswm);
}

/// `ra8_err_t ra8_eth_mfwd_set_forwarding_masks(const uint8_t port_masks[3])`.
export fn ra8_eth_mfwd_set_forwarding_masks(port_masks: ?*const [mfwd.port_count]u8) u16 {
    const masks = port_masks orelse {
        ra8_log_emit_error(tag, "set_forwarding_masks: null arg");
        return k_ra8_err_null_ptr;
    };
    mfwd.setForwardingMasks(window, masks);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_eth_mfwd_route_queue(uint8_t port, uint8_t queue_index)`.
export fn ra8_eth_mfwd_route_queue(port: u8, queue_index: u8) u16 {
    mfwd.routeQueue(window, port, queue_index) catch {
        ra8_log_emit_error(tag, "mfwd_route_queue: invalid port or queue");
        return k_ra8_err_invalid_arg;
    };
    return k_ra8_ok;
}
