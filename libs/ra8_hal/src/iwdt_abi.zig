//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_iwdt_* (internal/iwdt.zig, RA8FW-541). Built as its own object in
//! libra8_hal.a (RA8FW-542) so an image links only the units it calls.

const common = @import("abi_common.zig");
const iwdt = @import("internal/iwdt.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_info = common.ra8_log_emit_info;
const ra8_log_emit_error = common.ra8_log_emit_error;

const iwdt_tag = "IWDT";
var iwdt_handler: iwdt.Handler = .{};

/// `ra8_err_t ra8_iwdt_init(void)` (inc/ra8_iwdt.h). OFS0 starts the counter;
/// there is nothing to program.
export fn ra8_iwdt_init() u16 {
    ra8_log_emit_info(iwdt_tag, "iwdt_init (OFS0 controls period; auto-start only)");
    return k_ra8_ok;
}

/// `void ra8_iwdt_refresh_deferred(void)`.
export fn ra8_iwdt_refresh_deferred() void {
    iwdt.refresh(iwdt.hardware());
}

/// `ra8_err_t ra8_iwdt_get_status(uint16_t* out_mask)`.
export fn ra8_iwdt_get_status(out_mask: ?*u16) u16 {
    const out = out_mask orelse {
        ra8_log_emit_error(iwdt_tag, "out_mask must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    out.* = iwdt.status(iwdt.hardware());
    return k_ra8_ok;
}

/// `ra8_err_t ra8_iwdt_clear_status(void)`.
export fn ra8_iwdt_clear_status() u16 {
    iwdt.clearStatus(iwdt.hardware(), iwdt.status_all);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_iwdt_get_counter(uint16_t* out_counter)`.
export fn ra8_iwdt_get_counter(out_counter: ?*u16) u16 {
    const out = out_counter orelse {
        ra8_log_emit_error(iwdt_tag, "out_counter must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    out.* = iwdt.counter(iwdt.hardware());
    return k_ra8_ok;
}

/// `ra8_err_t ra8_iwdt_attach_handler(ra8_iwdt_event_fn_t fn, void* ctx)`.
export fn ra8_iwdt_attach_handler(func: ?iwdt.EventFn, ctx: ?*anyopaque) u16 {
    iwdt_handler = .{ .func = func, .ctx = ctx };
    return k_ra8_ok;
}

/// `void ra8_iwdt_dispatch(void)`: ISR-safe, no logging.
export fn ra8_iwdt_dispatch() void {
    iwdt.dispatch(iwdt.hardware(), iwdt_handler);
}
