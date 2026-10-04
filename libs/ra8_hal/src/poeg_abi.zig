//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the nine ra8_poeg_* functions in ra8_poeg.h (RA8FW-598). The
//! register sequences are in internal/poeg.zig; the per-group handler table
//! is this file's state, as the C static was.

const common = @import("abi_common.zig");
const poeg = @import("internal/poeg.zig");

const tag = "POEG";

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

var s_slots: [poeg.group_count]poeg.Slot = [_]poeg.Slot{.{}} ** poeg.group_count;

const Mmio = struct {
    fn reg(group: u8) *volatile u32 {
        return @ptrFromInt(poeg.base0 + @as(usize, group) * poeg.stride);
    }
    pub fn read(_: Mmio, group: u8) u32 {
        return reg(group).*;
    }
    pub fn write(_: Mmio, group: u8, value: u32) void {
        reg(group).* = value;
    }
};

const C = struct {
    pub fn mstpEnable(_: C, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn mstpDisable(_: C, id: u16) u16 {
        return ra8_mstp_disable(id);
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

export fn ra8_poeg_init(group: u8, cfg: ?*const poeg.Cfg) u16 {
    return poeg.init(Mmio{}, C{}, group, cfg);
}

export fn ra8_poeg_deinit(group: u8) u16 {
    return poeg.deinit(Mmio{}, C{}, &s_slots, group);
}

export fn ra8_poeg_trigger_stop(group: u8) u16 {
    return poeg.triggerStop(Mmio{}, C{}, group);
}

export fn ra8_poeg_get_status(group: u8, out_mask: ?*u32) u16 {
    return poeg.getStatus(Mmio{}, C{}, group, out_mask);
}

export fn ra8_poeg_clear_status(group: u8, mask: u32) u16 {
    return poeg.clearStatus(Mmio{}, C{}, group, mask);
}

export fn ra8_poeg_attach_handler(group: u8, handler: poeg.Handler, ctx: ?*anyopaque) u16 {
    return poeg.attachHandler(&s_slots, group, handler, ctx);
}

export fn ra8_poeg_enter_stop(group: u8) u16 {
    return poeg.enterStop(C{}, group);
}

export fn ra8_poeg_exit_stop(group: u8) u16 {
    return poeg.exitStop(C{}, group);
}

export fn ra8_poeg_dispatch(group: u8) void {
    poeg.dispatch(Mmio{}, &s_slots, group);
}
