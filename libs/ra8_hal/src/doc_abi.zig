//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the Data Operation Circuit (internal/doc.zig,
//! RA8FW-561). Built as its own object in libra8_hal.a (RA8FW-542).

const common = @import("abi_common.zig");
const doc = @import("internal/doc.zig");

const tag = "DOC";
const block = doc.Block{};

/// `k_ra8_mstp_doc`: (k_ra8_mstp_reg_c << 8) | 13 (inc/ra8_mstp_regs.h).
const mstp_doc: u16 = (2 << 8) | 13;
extern fn ra8_mstp_enable(id: u16) u16;

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

/// `ra8_err_t ra8_doc_init(void)`.
export fn ra8_doc_init() u16 {
    const err = ra8_mstp_enable(mstp_doc);
    if (err != common.k_ra8_ok) {
        common.ra8_log_emit_error(tag, "doc_init: mstp enable");
        common.ra8_log_emit_error_val(tag, "Error", err);
        return err;
    }
    block.reset();
    common.ra8_log_emit_info(tag, "doc_init");
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_doc_add16(uint16_t a, uint16_t b, uint16_t* out_sum)`.
export fn ra8_doc_add16(a: u16, b: u16, out_sum: ?*u16) u16 {
    const out = out_sum orelse return nullPtr("out_sum must not be nullptr");
    out.* = block.add16(a, b);
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_doc_sub16(uint16_t a, uint16_t b, uint16_t* out_diff)`.
export fn ra8_doc_sub16(a: u16, b: u16, out_diff: ?*u16) u16 {
    const out = out_diff orelse return nullPtr("out_diff must not be nullptr");
    out.* = block.sub16(a, b);
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_doc_set_window(uint16_t, uint16_t, ra8_doc_window_polarity_t)`.
export fn ra8_doc_set_window(lower: u16, upper: u16, polarity: u8) u16 {
    block.setWindow(lower, upper, polarity) catch |e| {
        common.ra8_log_emit_error(tag, switch (e) {
            error.BadRange => "set_window: lower must be strictly less than upper",
            error.BadPolarity => "set_window: polarity out of range",
        });
        return common.k_ra8_err_invalid_arg;
    };
    common.ra8_log_emit_info(tag, "set_window");
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_doc_window_compare(uint16_t value, bool* out_flag)`.
export fn ra8_doc_window_compare(value: u16, out_flag: ?*bool) u16 {
    const out = out_flag orelse return nullPtr("out_flag must not be nullptr");
    out.* = block.windowCompare(value) catch {
        common.ra8_log_emit_error(tag, "window_compare: DOC not in compare mode");
        return common.k_ra8_err_invalid_state;
    };
    return common.k_ra8_ok;
}
