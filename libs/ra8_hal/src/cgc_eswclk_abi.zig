//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_cgc_eswclk_init / ra8_cgc_eswclk_hz (RA8FW-584). The
//! HOCO-for-USB helper stays in ra8_cgc_usb.c.

const common = @import("abi_common.zig");
const eswclk = @import("internal/cgc_eswclk.zig");

const tag = "CGC";

extern fn priv_ra8_cgc_ensure_hoco_running_for_usb_ck() u16;
extern fn ra8_mstp_enable(id: u16) u16;

/// ESWCLK in Hz after the last successful init, 0 before.
var hz: u32 = 0;

/// R_SYSTEM byte and half-word registers by offset.
const Mmio = struct {
    pub fn read8(_: Mmio, off: usize) u8 {
        return @as(*volatile u8, @ptrFromInt(eswclk.system_base + off)).*;
    }
    pub fn write8(_: Mmio, off: usize, value: u8) void {
        @as(*volatile u8, @ptrFromInt(eswclk.system_base + off)).* = value;
    }
    pub fn write16(_: Mmio, off: usize, value: u16) void {
        @as(*volatile u16, @ptrFromInt(eswclk.system_base + off)).* = value;
    }
};

const C = struct {
    pub fn hoco(_: C) u16 {
        return priv_ra8_cgc_ensure_hoco_running_for_usb_ck();
    }
    pub fn mstpEnable(_: C, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn info(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn err(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

export fn ra8_cgc_eswclk_init() u16 {
    return eswclk.init(Mmio{}, C{}, &hz);
}

export fn ra8_cgc_eswclk_hz(out_hz: ?*u32) u16 {
    const out = out_hz orelse return common.k_ra8_err_null_ptr;
    out.* = hz;
    return common.k_ra8_ok;
}
