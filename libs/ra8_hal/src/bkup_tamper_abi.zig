//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_bkup_tamper_init / _disable, ra8_bkup_read_input and
//! ra8_bkup_set_input_enable (RA8FW-599). The shared domain state
//! s_bkup_initialized and priv_ra8_bkup_internal_rmw8 come from bkup_abi.zig.

const common = @import("abi_common.zig");
const tamper = @import("internal/bkup_tamper.zig");

/// Log tag, as `g_bkup_tag` was.
const tag = "BKUP";

extern var s_bkup_initialized: bool;
extern fn priv_ra8_bkup_internal_rmw8(reg: *volatile u8, mask: u8, enable: bool, unlock_val: u16) void;

fn reg8(off: usize) *volatile u8 {
    return @ptrFromInt(tamper.base + off);
}

const Mmio = struct {
    pub fn read(_: Mmio, off: usize) u8 {
        return reg8(off).*;
    }
    pub fn write(_: Mmio, off: usize, value: u8) void {
        reg8(off).* = value;
    }
    pub fn prcr(_: Mmio, value: u16) void {
        const p: *volatile u16 = @ptrFromInt(tamper.base + tamper.off_prcr);
        p.* = value;
    }
};

const C = struct {
    pub fn err(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn fail(_: C, msg: [*:0]const u8, code: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", code);
    }
    pub fn info(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn setInitialized(_: C) void {
        s_bkup_initialized = true;
    }
    pub fn rmw(_: C, off: usize, mask: u8, enable: bool, unlock_val: u16) void {
        priv_ra8_bkup_internal_rmw8(reg8(off), mask, enable, unlock_val);
    }
};

export fn ra8_bkup_tamper_init(cfg: ?*const tamper.Config) u16 {
    return tamper.init(Mmio{}, C{}, cfg);
}

export fn ra8_bkup_tamper_disable() u16 {
    return tamper.disable(Mmio{});
}

export fn ra8_bkup_read_input(channel: u8, high_out: ?*bool) u16 {
    return tamper.readInput(Mmio{}, C{}, channel, high_out);
}

export fn ra8_bkup_set_input_enable(channel: u8, enable: bool) u16 {
    return tamper.setInputEnable(C{}, channel, enable);
}
