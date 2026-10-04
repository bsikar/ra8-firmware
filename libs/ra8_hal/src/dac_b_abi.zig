//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_dac_b_* driver (RA8FW-585). The update-handler state
//! lives here; the register sequences are in internal/dac_b.zig.

const common = @import("abi_common.zig");
const dac = @import("internal/dac_b.zig");

const tag = "DAC_B";

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

/// `ra8_dac_b_update_fn_t`.
const UpdateFn = *const fn (ctx: ?*anyopaque, channel: u8) callconv(.C) void;

var handler: ?UpdateFn = null;
var handler_ctx: ?*anyopaque = null;

/// One DAC_B instance's registers at base0 + ch * stride.
const Mmio = struct {
    fn addr(ch: u8, off: usize) usize {
        return dac.base0 + @as(usize, ch) * dac.stride + off;
    }
    pub fn read32(_: Mmio, ch: u8, off: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(addr(ch, off))).*;
    }
    pub fn write32(_: Mmio, ch: u8, off: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(addr(ch, off))).* = value;
    }
    pub fn write16(_: Mmio, ch: u8, off: usize, value: u16) void {
        @as(*volatile u16, @ptrFromInt(addr(ch, off))).* = value;
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
    pub fn fail(_: C, msg: [*:0]const u8, err: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", err);
    }
};

export fn ra8_dac_b_init() u16 {
    return dac.init(Mmio{}, C{});
}

export fn ra8_dac_b_write(channel: u8, value: u16) u16 {
    return dac.write(Mmio{}, C{}, channel, value);
}

export fn ra8_dac_b_init_configured(cfg: ?*const dac.Cfg) u16 {
    const c = cfg orelse {
        common.ra8_log_emit_error(tag, "cfg must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    return dac.initConfigured(Mmio{}, C{}, c);
}

export fn ra8_dac_b_deinit() u16 {
    handler = null;
    handler_ctx = null;
    return dac.deinit(Mmio{}, C{});
}

export fn ra8_dac_b_set_vref(vref: u8) u16 {
    return dac.setVref(Mmio{}, vref);
}

export fn ra8_dac_b_set_output_enable(channel: u8, enable: bool) u16 {
    return dac.setOutputEnable(Mmio{}, channel, enable);
}

export fn ra8_dac_b_get_status(out_mask: ?*u8) u16 {
    const out = out_mask orelse {
        common.ra8_log_emit_error(tag, "out_mask must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    out.* = dac.status(Mmio{});
    return common.k_ra8_ok;
}

export fn ra8_dac_b_clear_status() u16 {
    return dac.clearStatus(Mmio{});
}

export fn ra8_dac_b_attach_handler(fn_ptr: ?UpdateFn, ctx: ?*anyopaque) u16 {
    handler = fn_ptr;
    handler_ctx = ctx;
    return common.k_ra8_ok;
}

export fn ra8_dac_b_enter_stop() u16 {
    return dac.enterStop(Mmio{}, C{});
}

export fn ra8_dac_b_exit_stop() u16 {
    return dac.exitStop(C{});
}

/// ISR-safe: reads the handler once and calls it for an in-range channel.
export fn ra8_dac_b_dispatch_update(channel: u8) void {
    if (channel >= dac.channel_count) return;
    const f = handler orelse return;
    f(handler_ctx, channel);
}
