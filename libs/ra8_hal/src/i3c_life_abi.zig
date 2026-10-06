//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for I3C init/deinit, attach_handler and dispatch (RA8FW-819).
//! Defines `s_i3c_chan`, which the other i3c_*_abi.zig units read through
//! ra8_i3c_internal.h. Register sequences are in internal/i3c_life.zig.

const common = @import("abi_common.zig");
const ctl = @import("internal/i3c_ctl.zig");
const life = @import("internal/i3c_life.zig");

const tag = "I3C";
/// `ra8_mstp_t` k_ra8_mstp_i3c: (k_ra8_mstp_reg_b << 8) | 4 (inc/ra8_mstp_regs.h).
const mstp_i3c: u16 = (1 << 8) | 4;
/// `k_ra8_i3c_mode_i2c`; any other mode takes the native path, as the C did.
const mode_i2c: u8 = 1;
/// `k_ra8_i3c_i2c_channel_count`.
const channel_count = 1;

/// `ra8_i3c_cfg_t`.
const Cfg = extern struct { mode: u8, bus_hz: u32, pclka_hz: u32 };
/// `ra8_i3c_i2c_cfg_t`.
const I2cCfg = extern struct { bus_hz: u32, pclka_hz: u32 };
/// `ra8_i3c_chan_state_t`.
const Chan = extern struct { initialized: bool = false, mode: u8 = 0 };
/// `ra8_i3c_event_fn_t`.
const EventFn = *const fn (ctx: ?*anyopaque, status: u32) callconv(.c) void;

/// Per-channel mode and init flag; the other I3C units and the C tests read it by this name.
export var s_i3c_chan: [channel_count]Chan = .{.{}};
var handler_fn: ?EventFn = null;
var handler_ctx: ?*anyopaque = null;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn ra8_i3c_i2c_init(channel: u8, cfg: *const I2cCfg) u16;
extern fn ra8_i3c_i2c_deinit(channel: u8) u16;
extern fn ra8_i3c_i2c_get_errors(channel: u8, out_mask: ?*u8) u16;
extern fn ra8_i3c_i2c_clear_errors(channel: u8) u16;

comptime {
    if (@sizeOf(Cfg) != 12) @compileError("ra8_i3c_cfg_t is 12 bytes");
    if (@sizeOf(Chan) != 2) @compileError("ra8_i3c_chan_state_t is 2 bytes");
}

const Mmio = struct {
    pub fn read32(_: Mmio, off: usize) u32 {
        const p: *volatile u32 = @ptrFromInt(ctl.base + off);
        return p.*;
    }
    pub fn write32(_: Mmio, off: usize, v: u32) void {
        const p: *volatile u32 = @ptrFromInt(ctl.base + off);
        p.* = v;
    }
};

export fn ra8_i3c_init(channel: u8, cfg: ?*const Cfg) u16 {
    const c = cfg orelse {
        common.ra8_log_emit_error(tag, "i3c_init: cfg");
        return common.k_ra8_err_null_ptr;
    };
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    if (c.mode == mode_i2c) {
        const e = ra8_i3c_i2c_init(channel, &.{ .bus_hz = c.bus_hz, .pclka_hz = c.pclka_hz });
        if (e != common.k_ra8_ok) return e;
    } else {
        const e = ra8_mstp_enable(mstp_i3c);
        if (e != common.k_ra8_ok) {
            common.ra8_log_emit_error(tag, "i3c_init: mstp enable");
            common.ra8_log_emit_error_val(tag, "Error", e);
            return e;
        }
        life.nativeInit(Mmio{});
    }
    s_i3c_chan[channel] = .{ .initialized = true, .mode = c.mode };
    common.ra8_log_emit_info(tag, "i3c_init");
    return common.k_ra8_ok;
}

export fn ra8_i3c_deinit(channel: u8) u16 {
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    var err: u16 = undefined;
    if (s_i3c_chan[channel].mode == mode_i2c) {
        err = ra8_i3c_i2c_deinit(channel);
    } else {
        life.nativeDeinit(Mmio{});
        handler_fn = null;
        handler_ctx = null;
        err = ra8_mstp_disable(mstp_i3c);
    }
    s_i3c_chan[channel].initialized = false;
    return err;
}

export fn ra8_i3c_attach_handler(channel: u8, f: ?EventFn, ctx: ?*anyopaque) u16 {
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    handler_fn = f;
    handler_ctx = ctx;
    return common.k_ra8_ok;
}

/// ISR-safe: snapshots the handler, then surfaces either the IIC_B error
/// mask (I2C-compat) or the latched INST flags (native).
export fn ra8_i3c_dispatch(channel: u8) void {
    if (channel >= channel_count) return;
    const f = handler_fn;
    const ctx = handler_ctx;
    var mask: u32 = undefined;
    if (s_i3c_chan[channel].mode == mode_i2c) {
        var m: u8 = 0;
        _ = ra8_i3c_i2c_get_errors(channel, &m);
        _ = ra8_i3c_i2c_clear_errors(channel);
        mask = m;
    } else {
        mask = life.takeStatus(Mmio{});
    }
    if (f) |call| call(ctx, mask);
}
