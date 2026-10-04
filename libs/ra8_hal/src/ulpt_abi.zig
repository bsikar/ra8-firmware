//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ten ra8_ulpt_* functions in ra8_ulpt.h (RA8FW-597). The
//! register sequences are in internal/ulpt.zig; the event handler and its
//! context are this file's state, as the C statics were.

const common = @import("abi_common.zig");
const ulpt = @import("internal/ulpt.zig");

const tag = "ULPT";

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

var s_fn: ulpt.Handler = null;
var s_ctx: ?*anyopaque = null;

const Mmio = struct {
    fn addr(ch: u8, off: usize) usize {
        return ulpt.base0 + @as(usize, ch) * ulpt.stride + off;
    }
    pub fn read8(_: Mmio, ch: u8, off: usize) u8 {
        return @as(*volatile u8, @ptrFromInt(addr(ch, off))).*;
    }
    pub fn write8(_: Mmio, ch: u8, off: usize, value: u8) void {
        @as(*volatile u8, @ptrFromInt(addr(ch, off))).* = value;
    }
    pub fn write32(_: Mmio, ch: u8, off: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(addr(ch, off))).* = value;
    }
};

const C = struct {
    pub fn mstpEnable(_: C, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn mstpDisable(_: C, id: u16) u16 {
        return ra8_mstp_disable(id);
    }
    pub fn info(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn infoVal(_: C, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_info_val(tag, msg, value);
    }
    pub fn err(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn fail(_: C, msg: [*:0]const u8, code: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", code);
    }
};

export fn ra8_ulpt_init() u16 {
    return ulpt.init(Mmio{}, C{});
}

export fn ra8_ulpt_start(channel: u8, period: u32) u16 {
    return ulpt.start(Mmio{}, C{}, channel, period);
}

export fn ra8_ulpt_stop(channel: u8) u16 {
    return ulpt.stop(Mmio{}, channel);
}

export fn ra8_ulpt_deinit(channel: u8) u16 {
    return ulpt.deinit(Mmio{}, C{}, channel);
}

export fn ra8_ulpt_set_period(channel: u8, period: u32) u16 {
    return ulpt.setPeriod(Mmio{}, channel, period);
}

export fn ra8_ulpt_get_status(channel: u8, out_mask: ?*u8) u16 {
    return ulpt.getStatus(Mmio{}, C{}, channel, out_mask);
}

export fn ra8_ulpt_attach_handler(handler: ulpt.Handler, ctx: ?*anyopaque) u16 {
    s_fn = handler;
    s_ctx = ctx;
    return common.k_ra8_ok;
}

export fn ra8_ulpt_dispatch(channel: u8) void {
    ulpt.dispatch(s_fn, s_ctx, channel);
}

export fn ra8_ulpt_enter_stop(channel: u8) u16 {
    return ulpt.enterStop(C{}, channel);
}

export fn ra8_ulpt_exit_stop(channel: u8) u16 {
    return ulpt.exitStop(C{}, channel);
}
