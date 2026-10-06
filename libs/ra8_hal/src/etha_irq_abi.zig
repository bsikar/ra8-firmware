//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ETHA error-IRQ entry points (RA8FW-815). The per-port slots
//! stay defined in ra8_etha.c; the logic is in internal/etha_irq.zig.

const common = @import("abi_common.zig");
const irq = @import("internal/etha_irq.zig");

const tag = "ETHA";
const port_bases = [_]usize{ 0x403C_A000, 0x403C_C000 };

/// `ra8_etha_event_fn_t`.
const EventFn = *const fn (ctx: ?*anyopaque, port: u8, s0: u32, s1: u32, s2: u32) callconv(.C) void;
/// `ra8_etha_slot_t`; stats (28 B) is not touched here.
const Slot = extern struct { cb: ?EventFn, ctx: ?*anyopaque, stats: [7]u32 };

extern var s_etha_slots: [port_bases.len]Slot;

comptime {
    if (@sizeOf(irq.Status) != 20) @compileError("ra8_etha_status_t is 20 bytes");
    if (@offsetOf(irq.Status, "eaeis0") != 4) @compileError("status.eaeis0 sits at +4");
}

const Mmio = struct {
    base: usize,
    pub fn read32(self: Mmio, off: usize) u32 {
        const p: *volatile u32 = @ptrFromInt(self.base + off);
        return p.*;
    }
    pub fn write32(self: Mmio, off: usize, v: u32) void {
        const p: *volatile u32 = @ptrFromInt(self.base + off);
        p.* = v;
    }
};

fn fail(code: u16, msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return code;
}

fn regs(p: u8) Mmio {
    return .{ .base = port_bases[p] };
}

fn argsOk(p: u8, block: u8) bool {
    return p < port_bases.len and irq.blockOk(block);
}

export fn ra8_etha_get_status(p: u8, out: ?*irq.Status) u16 {
    const o = out orelse return fail(common.k_ra8_err_null_ptr, "etha_get_status: out must not be nullptr");
    if (p >= port_bases.len) return fail(common.k_ra8_err_invalid_arg, "etha_get_status: port out of range");
    o.* = irq.status(regs(p));
    return common.k_ra8_ok;
}

export fn ra8_etha_clear_status(p: u8, block: u8, mask: u32) u16 {
    if (!argsOk(p, block)) return fail(common.k_ra8_err_invalid_arg, "etha_clear_status: port/block out of range");
    irq.clear(regs(p), block, mask);
    return common.k_ra8_ok;
}

export fn ra8_etha_enable_irq(p: u8, block: u8, mask: u32) u16 {
    if (!argsOk(p, block)) return fail(common.k_ra8_err_invalid_arg, "etha_enable_irq: bad arg");
    irq.enable(regs(p), block, mask);
    return common.k_ra8_ok;
}

export fn ra8_etha_disable_irq(p: u8, block: u8, mask: u32) u16 {
    if (!argsOk(p, block)) return fail(common.k_ra8_err_invalid_arg, "etha_disable_irq: bad arg");
    irq.disable(regs(p), block, mask);
    return common.k_ra8_ok;
}

export fn ra8_etha_attach_handler(p: u8, cb: ?EventFn, ctx: ?*anyopaque) u16 {
    if (p >= port_bases.len) return fail(common.k_ra8_err_invalid_arg, "etha_attach_handler: port out of range");
    s_etha_slots[p].cb = cb;
    s_etha_slots[p].ctx = ctx;
    return common.k_ra8_ok;
}

export fn ra8_etha_dispatch(p: u8) void {
    if (p >= port_bases.len) return;
    const s = irq.dispatch(regs(p));
    const slot = s_etha_slots[p];
    if (slot.cb) |f| f(slot.ctx, p, s[0], s[1], s[2]);
}
