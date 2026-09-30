//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_scb.h` (#2868).
//!
//! The register window below deals in named registers; this file maps it
//! onto the `ra8_err_t` codes the header promises and keeps the one logged
//! rejection the C had, through `ra8_log_emit_error`.

const scb = @import("fault_scb");

/// `ra8_err_t` values this module returns, from `inc/ra8_err.h`.
const err = struct {
    pub const ok: c_int = 0;
    pub const null_ptr: c_int = 0x504;
};

const tag: [*:0]const u8 = "ra8_scb";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

pub export fn ra8_scb_read_fault_status(out: ?*scb.FaultStatus) callconv(.c) c_int {
    const slot = out orelse {
        ra8_log_emit_error(tag, "read_fault_status: out");
        return err.null_ptr;
    };
    slot.* = scb.readFaultStatus();
    return err.ok;
}

pub export fn ra8_scb_set_vtor(base: usize) callconv(.c) void {
    scb.setVtor(base);
}

pub export fn ra8_scb_get_vtor() callconv(.c) usize {
    return scb.getVtor();
}

pub export fn ra8_scb_trace_enabled() callconv(.c) bool {
    return scb.traceEnabled();
}

pub export fn ra8_scb_trace_enable() callconv(.c) void {
    scb.traceEnable();
}
