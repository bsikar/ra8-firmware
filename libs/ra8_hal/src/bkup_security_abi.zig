//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the battery-backup security attribution
//! (internal/bkup_security.zig, RA8FW-555). Built as its own object in
//! libra8_hal.a (RA8FW-542). Log lines match the deleted
//! ra8_bkup_security.c, including RA8_RETURN_ON_ERROR's message-then-
//! "Error" pair.

const common = @import("abi_common.zig");
const sec = @import("internal/bkup_security.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_error = common.ra8_log_emit_error;
const ra8_log_emit_error_val = common.ra8_log_emit_error_val;

/// `g_bkup_tag` (src/ra8_bkup.c).
const tag = "BKUP";
const null_msg = "security cfg must not be nullptr";

fn block() sec.Block {
    return .{ .base = sec.base };
}

/// `RA8_RETURN_ON_ERROR` with invalid_arg.
fn logReturn(msg: [*:0]const u8) void {
    ra8_log_emit_error(tag, msg);
    ra8_log_emit_error_val(tag, "Error", k_ra8_err_invalid_arg);
}

/// The inner per-boundary line the C validator logged, if any.
fn innerMessage(err: sec.Error) ?[*:0]const u8 {
    return switch (err) {
        error.BadBbfsar => null,
        error.BadSaba => "security_apply: saba bad",
        error.BadPabas => "security_apply: pabas bad",
        error.BadPabans => "security_apply: pabans bad",
    };
}

/// `ra8_err_t ra8_bkup_security_apply(const ra8_bkup_security_config_t* cfg)`.
export fn ra8_bkup_security_apply(cfg: ?*const sec.Config) u16 {
    const c = cfg orelse {
        ra8_log_emit_error(tag, null_msg);
        return k_ra8_err_null_ptr;
    };
    sec.apply(block(), c.*) catch |err| {
        if (innerMessage(err)) |msg| logReturn(msg);
        logReturn("security_apply: cfg bad");
        return k_ra8_err_invalid_arg;
    };
    return k_ra8_ok;
}

/// `ra8_err_t ra8_bkup_security_get(ra8_bkup_security_config_t* cfg)`.
export fn ra8_bkup_security_get(cfg: ?*sec.Config) u16 {
    const c = cfg orelse {
        ra8_log_emit_error(tag, null_msg);
        return k_ra8_err_null_ptr;
    };
    c.* = sec.get(block());
    return k_ra8_ok;
}
