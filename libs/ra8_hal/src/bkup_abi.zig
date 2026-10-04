//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_bkup.h's core functions (RA8FW-601). The register
//! sequences are in internal/bkup.zig. This unit also owns the backup
//! domain's shared state, exported as C symbols because bkup_tamper_abi.zig
//! is a separate root module: s_bkup_initialized and
//! priv_ra8_bkup_internal_rmw8.

const common = @import("abi_common.zig");
const bkup = @import("internal/bkup.zig");

const tag = "BKUP";
/// `k_ra8_bkup_vbae_settle_iters`: >= 500 ns at 1 GHz (HUM Ch 12.2.6).
const vbae_settle_iters: u32 = 1000;

pub const EventFn = *const fn (ctx: ?*anyopaque, tamper_flags: u8) callconv(.c) void;

export var s_bkup_initialized: bool = false;
var s_bkup_fn: ?EventFn = null;
var s_bkup_ctx: ?*anyopaque = null;

const Mmio = struct {
    fn ptr(comptime T: type, off: usize) *volatile T {
        return @ptrFromInt(bkup.base + off);
    }
    pub fn read8(_: Mmio, off: usize) u8 {
        return ptr(u8, off).*;
    }
    pub fn write8(_: Mmio, off: usize, value: u8) void {
        ptr(u8, off).* = value;
    }
    pub fn read32(_: Mmio, off: usize) u32 {
        return ptr(u32, off).*;
    }
    pub fn write32(_: Mmio, off: usize, value: u32) void {
        ptr(u32, off).* = value;
    }
    pub fn prcr(_: Mmio, value: u16) void {
        ptr(u16, bkup.off_prcr).* = value;
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
    pub fn settle(_: C) void {
        var i: u32 = 0;
        while (i < vbae_settle_iters) : (i += 1) asm volatile ("nop");
    }
    pub fn setInitialized(_: C, value: bool) void {
        s_bkup_initialized = value;
    }
    pub fn isInitialized(_: C) bool {
        return s_bkup_initialized;
    }
    pub fn dispatch(_: C, flags: u8) void {
        ra8_bkup_dispatch(flags);
    }
};

/// Read-modify-write one PRCR-protected byte register (shared with
/// bkup_tamper_abi.zig).
export fn priv_ra8_bkup_internal_rmw8(reg: *volatile u8, mask: u8, enable: bool, unlock_val: u16) void {
    bkup.rmw8(Mmio{}, @intFromPtr(reg) - bkup.base, mask, enable, unlock_val);
}

export fn ra8_bkup_init(cfg: ?*const bkup.Config) u16 {
    return bkup.init(Mmio{}, C{}, cfg);
}
export fn ra8_bkup_deinit() u16 {
    return bkup.deinit(Mmio{}, C{});
}
export fn ra8_bkup_cold_start_init(level: u8, timeout_iters: u32) u16 {
    return bkup.coldStartInit(Mmio{}, C{}, level, timeout_iters);
}
export fn ra8_bkup_warm_start_check(needs_reinit: ?*bool, timeout_iters: u32) u16 {
    return bkup.warmStartCheck(Mmio{}, C{}, needs_reinit, timeout_iters);
}
export fn ra8_bkup_no_switch_init(timeout_iters: u32) u16 {
    return bkup.noSwitchInit(Mmio{}, C{}, timeout_iters);
}
export fn ra8_bkup_get_status(out: ?*bkup.Status) u16 {
    return bkup.getStatus(Mmio{}, C{}, out);
}
export fn ra8_bkup_clear_status(mask: u8) u16 {
    return bkup.clearStatus(Mmio{}, mask);
}
export fn ra8_bkup_read_word(word_index: u8, out: ?*u32) u16 {
    return bkup.readWord(Mmio{}, C{}, word_index, out);
}
export fn ra8_bkup_write_word(word_index: u8, value: u32) u16 {
    return bkup.writeWord(Mmio{}, word_index, value);
}
export fn ra8_bkup_read_byte(index: u16, out: ?*u8) u16 {
    return bkup.readByte(Mmio{}, C{}, index, out);
}
export fn ra8_bkup_write_byte(index: u16, value: u8) u16 {
    return bkup.writeByte(Mmio{}, index, value);
}
export fn ra8_bkup_zero_all() u16 {
    return bkup.zeroAll(Mmio{});
}
export fn ra8_bkup_set_voltage_monitor(enable: bool) u16 {
    return bkup.setVoltageMonitor(Mmio{}, enable);
}
export fn ra8_bkup_get_voltage_monitor_enabled(enabled_out: ?*bool) u16 {
    return bkup.getVoltageMonitor(Mmio{}, C{}, enabled_out);
}
export fn ra8_bkup_attach_handler(func: ?EventFn, ctx: ?*anyopaque) u16 {
    s_bkup_fn = func;
    s_bkup_ctx = ctx;
    return bkup.ok;
}
export fn ra8_bkup_isr_handle() u16 {
    return bkup.isrHandle(Mmio{}, C{});
}
export fn ra8_bkup_dispatch(tamper_flags: u8) void {
    if (s_bkup_fn) |func| func(s_bkup_ctx, tamper_flags);
}
