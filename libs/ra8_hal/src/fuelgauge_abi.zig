//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_fuelgauge_* (internal/fuelgauge.zig, RA8FW-553).
//! Built as its own object in libra8_hal.a (RA8FW-542).

const common = @import("abi_common.zig");
const fg = @import("internal/fuelgauge.zig");
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_error = common.ra8_log_emit_error;

const tag = "FUELGAUGE";

fn nullPtr(msg: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, msg);
    return k_ra8_err_null_ptr;
}

/// `ra8_err_t ra8_fuelgauge_open(ra8_fuelgauge_t* fg, const ra8_fuelgauge_cfg_t* cfg)`.
export fn ra8_fuelgauge_open(handle: ?*fg.Handle, cfg: ?*const fg.Cfg) u16 {
    const h = handle orelse return nullPtr("fg is NULL");
    const c = cfg orelse return nullPtr("cfg is NULL");
    return @intFromEnum(fg.open(h, c));
}

/// `ra8_err_t ra8_fuelgauge_read(ra8_fuelgauge_t* fg, ra8_fuelgauge_state_t* out)`.
export fn ra8_fuelgauge_read(handle: ?*fg.Handle, out: ?*fg.State) u16 {
    const h = handle orelse return nullPtr("fg is NULL");
    const o = out orelse return nullPtr("out is NULL");
    return @intFromEnum(fg.read(h, o));
}

/// `ra8_err_t ra8_fuelgauge_close(ra8_fuelgauge_t* fg)`.
export fn ra8_fuelgauge_close(handle: ?*fg.Handle) u16 {
    const h = handle orelse return nullPtr("fg is NULL");
    return @intFromEnum(fg.close(h));
}
