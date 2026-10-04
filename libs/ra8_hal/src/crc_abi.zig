//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for inc/ra8_crc.h. Module-stop control stays in ra8_mstp (C).

const common = @import("abi_common.zig");
const crc = @import("internal/crc.zig");

const tag = "CRC";
const block = crc.Block{};
/// k_ra8_mstp_crc: MSTPCRC bit 1 (k_ra8_mstp_reg_c = 2).
const mstp_crc: u16 = (2 << 8) | 1;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

export fn ra8_crc_init(poly: u8) u16 {
    const err = ra8_mstp_enable(mstp_crc);
    if (err != common.k_ra8_ok) {
        common.ra8_log_emit_error(tag, "crc_init: mstp enable");
        common.ra8_log_emit_error_val(tag, "Error", err);
        return err;
    }
    block.select(poly);
    block.snoopOff();
    common.ra8_log_emit_info_val(tag, "crc_init poly", poly);
    return common.k_ra8_ok;
}

export fn ra8_crc_reset() void {
    block.reset();
}

export fn ra8_crc_compute(data: ?[*]const u8, len: u32, out_crc: ?*u32) u16 {
    const bytes = data orelse {
        common.ra8_log_emit_error(tag, "data must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    const out = out_crc orelse {
        common.ra8_log_emit_error(tag, "out_crc must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    out.* = block.compute(bytes[0..len]);
    return common.k_ra8_ok;
}

export fn ra8_crc_deinit() u16 {
    block.clear();
    return ra8_mstp_disable(mstp_crc);
}

export fn ra8_crc_set_poly(poly: u8) u16 {
    block.select(poly);
    return common.k_ra8_ok;
}

export fn ra8_crc_get_status(out_poly: ?*u8) u16 {
    const out = out_poly orelse {
        common.ra8_log_emit_error(tag, "out_poly must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    out.* = block.crccr0();
    return common.k_ra8_ok;
}

export fn ra8_crc_enter_stop() u16 {
    block.stop();
    return ra8_mstp_disable(mstp_crc);
}

export fn ra8_crc_exit_stop() u16 {
    return ra8_mstp_enable(mstp_crc);
}
