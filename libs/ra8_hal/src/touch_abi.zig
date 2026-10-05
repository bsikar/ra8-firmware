//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_touch.h (RA8FW-790), replacing ra8_touch.c. The GT911 is
//! reached through the injected ra8_i2c_bus_ops_t seam.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const t = @import("internal/touch.zig");

const tag = "TOUCH";
const ok = common.k_ra8_ok;

const WriteFn = *const fn (?*anyopaque, u8, [*]const u8, u32, bool) callconv(.C) u16;
const ReadFn = *const fn (?*anyopaque, u8, [*]u8, u32) callconv(.C) u16;
const TransferFn = *const fn (?*anyopaque, u8, [*]const u8, u32, [*]u8, u32) callconv(.C) u16;
const EventFn = *const fn (?*anyopaque) callconv(.C) void;

/// ra8_i2c_bus_ops_t: write, read, transfer, ctx.
const BusOps = extern struct { write: ?WriteFn, read: ?ReadFn, transfer: ?TransferFn, ctx: ?*anyopaque };

/// ra8_touch_cfg_t.
const Cfg = extern struct { bus: BusOps, target_7b: u8, irq_pin: u8, max_points: u8 };

/// ra8_icu_irq_cfg_t.
const IcuCfg = extern struct { sense: u8, filter_div: u8, filter_en: bool };

comptime {
    const p = @sizeOf(usize);
    if (@offsetOf(Cfg, "target_7b") != 4 * p) @compileError("Cfg.target_7b");
    if (@offsetOf(Cfg, "max_points") != 4 * p + 2) @compileError("Cfg.max_points");
}

extern fn ra8_icu_configure_irq_pin(irq_num: u8, cfg: *const IcuCfg) u16;

const State = struct {
    cb: ?EventFn = null,
    ctx: ?*anyopaque = null,
    bus: BusOps = .{ .write = null, .read = null, .transfer = null, .ctx = null },
    target_7b: u8 = 0,
    max_points: u8 = 0,
    irq_pin: u8 = 0,
    opened: bool = false,
};

var state: State = .{};

/// The live bus: register reads go through transfer, writes through write.
const Bus = struct {
    pub fn read(_: Bus, reg: u16, buf: []u8) u16 {
        const r = t.packReg(reg);
        return state.bus.transfer.?(state.bus.ctx, state.target_7b, &r, r.len, buf.ptr, @intCast(buf.len));
    }
    pub fn writeByte(_: Bus, reg: u16, value: u8) u16 {
        const r = t.packReg(reg);
        const payload = [_]u8{ r[0], r[1], value };
        return state.bus.write.?(state.bus.ctx, state.target_7b, &payload, payload.len, true);
    }
};

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

/// RA8_RETURN_ON_ERROR: the message, then "Error" with the code.
fn failed(err: u16, msg: [*:0]const u8) bool {
    if (err == ok) return false;
    common.ra8_log_emit_error(tag, msg);
    common.ra8_log_emit_error_val(tag, "Error", err);
    return true;
}

fn validate(cfg: *const Cfg) u16 {
    if (cfg.bus.write == null or cfg.bus.transfer == null) return common.k_ra8_err_invalid_arg;
    if (!t.validAddr(cfg.target_7b)) return common.k_ra8_err_invalid_arg;
    return ok;
}

/// Pins at or above 32 (the unset sentinel included) mean polling only.
fn attachIrqPin(pin: u8) u16 {
    if (pin >= t.irq_pin_count) return ok;
    const icu = IcuCfg{ .sense = 0, .filter_div = 0, .filter_en = false };
    return ra8_icu_configure_irq_pin(pin, &icu);
}

fn finalise(cfg: *const Cfg) u16 {
    const bus = Bus{};
    const pid = t.checkProductId(bus);
    if (pid != ok) return pid;
    _ = bus.writeByte(t.reg_status, t.cmd_clear_status);
    return attachIrqPin(cfg.irq_pin);
}

export fn ra8_touch_open(cfg: ?*const Cfg) u16 {
    const c = cfg orelse return nullPtr("ra8_touch_open: cfg");
    if (state.opened) return common.k_ra8_err_invalid_state;
    if (failed(validate(c), "ra8_touch_open: cfg validation")) return validate(c);
    state.bus = c.bus;
    state.target_7b = c.target_7b;
    state.irq_pin = c.irq_pin;
    state.max_points = t.clampCap(c.max_points);
    const fin = finalise(c);
    if (failed(fin, "ra8_touch_open: finalise")) return fin;
    state.cb = null;
    state.ctx = null;
    state.opened = true;
    common.ra8_log_emit_info(tag, "ra8_touch_open");
    return ok;
}

export fn ra8_touch_close() u16 {
    if (!state.opened) return common.k_ra8_err_not_initialized;
    state.cb = null;
    state.ctx = null;
    state.opened = false;
    return ok;
}

export fn ra8_touch_attach_handler(fn_ptr: ?EventFn, ctx: ?*anyopaque) u16 {
    if (!state.opened) return common.k_ra8_err_not_initialized;
    state.cb = fn_ptr;
    state.ctx = ctx;
    return ok;
}

export fn ra8_touch_dispatch_irq() void {
    const f = state.cb;
    const ctx = state.ctx;
    if (f) |call| call(ctx);
}

export fn ra8_touch_read(out_points: ?[*]t.Point, max_count: u8, got_count: ?*u8) u16 {
    const out = out_points orelse return nullPtr("ra8_touch_read: out_points");
    const got = got_count orelse return nullPtr("ra8_touch_read: got_count");
    if (max_count == 0) {
        got.* = 0;
        return common.k_ra8_err_invalid_arg;
    }
    if (!state.opened) {
        got.* = 0;
        return common.k_ra8_err_not_initialized;
    }
    return t.readFrame(Bus{}, out, max_count, state.max_points, got);
}

/// The GT911 is factory-calibrated.
export fn ra8_touch_calibrate() u16 {
    return ok;
}

/// The C kept this under UNIT_TEST; only hosted builds carry it.
fn testDecode(raw: ?[*]const u8, n_points: u8, out_points: ?[*]t.Point, max_count: u8, got_count: ?*u8) callconv(.C) u16 {
    const r = raw orelse return nullPtr("test_decode: raw");
    const out = out_points orelse return nullPtr("test_decode: out_points");
    const got = got_count orelse return nullPtr("test_decode: got_count");
    if (max_count == 0) {
        got.* = 0;
        return common.k_ra8_err_invalid_arg;
    }
    t.decodeBlock(r, n_points, out, max_count, got);
    return ok;
}

comptime {
    if (builtin.os.tag != .freestanding) @export(&testDecode, .{ .name = "ra8_touch_test_decode" });
}
