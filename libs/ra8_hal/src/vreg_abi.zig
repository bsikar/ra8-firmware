//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the internal voltage regulator (RA8FW-788), which replaces
//! ra8_vreg.c. Same 17 symbols, register write order, error codes and
//! log strings. Logic is in internal/vreg.zig.

const common = @import("abi_common.zig");
const v = @import("internal/vreg.zig");

const tag = "VREG";
const ok = v.ok;
const sysc_base: usize = 0x4001E000;

const Mmio = struct {
    fn reg(off: u16) *volatile u8 {
        return @ptrFromInt(sysc_base + off);
    }
    pub fn read(_: Mmio, off: u16) u8 {
        return reg(off).*;
    }
    pub fn write(_: Mmio, off: u16, value: u8) void {
        reg(off).* = value;
    }
};

const EventFn = *const fn (?*anyopaque, u8) callconv(.c) void;

const hw = Mmio{};
var state: v.State = .{};
var handler: ?EventFn = null;
var handler_ctx: ?*anyopaque = null;

export fn ra8_vreg_init(cfg_ptr: ?*const v.Cfg) u16 {
    const cfg = cfg_ptr orelse {
        common.ra8_log_emit_error(tag, "cfg must not be nullptr");
        return v.err_null_ptr;
    };
    if (v.validate(cfg)) |e| return e;
    hw.write(v.off_lvocr, 0);
    hw.write(v.off_vccsel, cfg.vccsel & v.vccsel_mask);
    if (cfg.mode == v.mode_dcdc) {
        state.dcdcctl = v.enableDcdc(hw, cfg.fast_startup);
    } else {
        const ldo = v.packLdo(cfg);
        hw.write(v.off_dcdcctl, ldo);
        state.dcdcctl = ldo;
    }
    const lvocr = v.lvocrOf(cfg.lv_profile);
    hw.write(v.off_lvocr, lvocr);
    state = .{
        .dcdcctl = state.dcdcctl,
        .vccsel = cfg.vccsel,
        .lvocr = lvocr,
        .ocp = cfg.ocp,
        .lv_profile = cfg.lv_profile,
        .last_standby = v.standby_software,
        .initialized = true,
        .fast_startup = cfg.fast_startup,
        .ldo_boost = cfg.ldo_boost,
    };
    common.ra8_log_emit_info(tag, "vreg_init");
    return ok;
}

export fn ra8_vreg_deinit() u16 {
    hw.write(v.off_dcdcctl, 0);
    hw.write(v.off_vccsel, 0);
    hw.write(v.off_lvocr, 0);
    state = .{ .last_standby = state.last_standby };
    return ok;
}

export fn ra8_vreg_set_mode(mode: u8) u16 {
    if (mode != v.mode_ldo and mode != v.mode_dcdc) return v.err_invalid_arg;
    if (mode == v.mode_dcdc) {
        state.dcdcctl = v.enableDcdc(hw, state.fast_startup);
    } else {
        v.disableDcdc(hw, state.ldo_boost);
        state.dcdcctl = if (state.ldo_boost) v.lcboost else 0;
    }
    return ok;
}

export fn ra8_vreg_set_vccsel(sel: u8) u16 {
    if (sel > v.vccsel_max) return v.err_invalid_arg;
    hw.write(v.off_vccsel, sel & v.vccsel_mask);
    state.vccsel = sel;
    return ok;
}

/// Read-modify-write one DCDCCTL bit and cache the result.
fn rmw(mask: u8, on: bool) void {
    const cur = v.setBit(hw.read(v.off_dcdcctl), mask, on);
    hw.write(v.off_dcdcctl, cur);
    state.dcdcctl = cur;
}

export fn ra8_vreg_set_ocp(level: u8) u16 {
    if (level > v.ocp_high) return v.err_invalid_arg;
    rmw(v.ocpen, level != v.ocp_off);
    state.ocp = level;
    return ok;
}

export fn ra8_vreg_set_fast_startup(enable: bool) u16 {
    rmw(v.fst, enable);
    state.fast_startup = enable;
    return ok;
}

export fn ra8_vreg_set_ldo_boost(enable: bool) u16 {
    rmw(v.lcboost, enable);
    state.ldo_boost = enable;
    return ok;
}

export fn ra8_vreg_set_lv_profile(profile: u8) u16 {
    if (profile > v.lv_p1) return v.err_invalid_arg;
    const bits = v.lvocrOf(profile);
    hw.write(v.off_lvocr, bits);
    state.lvocr = bits;
    state.lv_profile = profile;
    return ok;
}

export fn ra8_vreg_get_status(out: ?*v.Status) u16 {
    const dst = out orelse {
        common.ra8_log_emit_error(tag, "out must not be nullptr");
        return v.err_null_ptr;
    };
    const dcdcctl = hw.read(v.off_dcdcctl);
    const vccsel = hw.read(v.off_vccsel);
    const lvocr = hw.read(v.off_lvocr);
    dst.* = v.decode(dcdcctl, vccsel, lvocr, state.ocp);
    return ok;
}

export fn ra8_vreg_clear_status(mask: u8) u16 {
    if (mask & ~v.dcdcctl_all != 0) return v.err_invalid_arg;
    const next = hw.read(v.off_dcdcctl) & ~mask;
    hw.write(v.off_dcdcctl, next);
    state.dcdcctl = next;
    return ok;
}

export fn ra8_vreg_reset() u16 {
    const err = ra8_vreg_deinit();
    state.last_standby = v.standby_software;
    return err;
}

export fn ra8_vreg_enter_standby(variant: u8) u16 {
    if (variant > v.standby_max) return v.err_invalid_arg;
    state.last_standby = variant;
    v.disableDcdc(hw, false);
    return ok;
}

export fn ra8_vreg_enter_stop() u16 {
    return ra8_vreg_enter_standby(v.standby_software);
}

export fn ra8_vreg_exit_standby() u16 {
    if (!state.initialized) return ok;
    hw.write(v.off_vccsel, state.vccsel & v.vccsel_mask);
    hw.write(v.off_dcdcctl, state.dcdcctl);
    hw.write(v.off_lvocr, state.lvocr);
    return ok;
}

export fn ra8_vreg_exit_stop() u16 {
    return ra8_vreg_exit_standby();
}

export fn ra8_vreg_attach_handler(fn_ptr: ?EventFn, ctx: ?*anyopaque) u16 {
    handler = fn_ptr;
    handler_ctx = ctx;
    return ok;
}

export fn ra8_vreg_dispatch() void {
    const f = handler orelse return;
    f(handler_ctx, hw.read(v.off_dcdcctl));
}
