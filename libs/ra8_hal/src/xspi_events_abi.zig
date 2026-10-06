//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for XSPI status, handler, dispatch and stop (RA8FW-865). Owns the
//! per-instance callback table under its C name; xspi_init_abi.zig (deinit) clears
//! and clears it through an extern declaration.

const common = @import("abi_common.zig");
const ev = @import("internal/xspi_events.zig");

const tag = "XSPI";

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

export var s_xspi_state: [ev.instance_count]ev.State = @splat(.{});

const Regs = struct {
    base: usize,

    pub fn read(self: Regs, off: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(self.base + off)).*;
    }
    pub fn write(self: Regs, off: usize, v: u32) void {
        @as(*volatile u32, @ptrFromInt(self.base + off)).* = v;
    }
};

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

fn regs(instance: u8) ?Regs {
    const b = ev.instanceBase(instance) orelse return null;
    return .{ .base = b };
}

export fn ra8_xspi_get_status(instance: u8, out_mask: ?*u32) u16 {
    const out = out_mask orelse return nullPtr("out_mask must not be nullptr");
    const r = regs(instance) orelse return nullPtr("instance out of range");
    out.* = r.read(ev.off_comstt);
    return 0;
}

export fn ra8_xspi_clear_status(instance: u8, mask: u32) u16 {
    const r = regs(instance) orelse return nullPtr("instance out of range");
    r.write(ev.off_intc, mask);
    return 0;
}

export fn ra8_xspi_attach_handler(instance: u8, func: ?ev.EventFn, ctx: ?*anyopaque) u16 {
    if (!ev.inRange(instance)) return common.k_ra8_err_invalid_arg;
    ev.attach(&s_xspi_state[instance], func, ctx);
    return 0;
}

export fn ra8_xspi_dispatch(instance: u8) void {
    const r = regs(instance) orelse return;
    ev.dispatch(r, s_xspi_state[instance]);
}

export fn ra8_xspi_enter_stop(instance: u8) u16 {
    if (!ev.inRange(instance)) return common.k_ra8_err_invalid_arg;
    return ra8_mstp_disable(ev.mstp_ids[instance]);
}

export fn ra8_xspi_exit_stop(instance: u8) u16 {
    if (!ev.inRange(instance)) return common.k_ra8_err_invalid_arg;
    return ra8_mstp_enable(ev.mstp_ids[instance]);
}
