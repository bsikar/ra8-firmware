//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ETHA lifecycle and mode entry points (RA8FW-817). Defines
//! the per-port slots shared with etha_stats_abi/etha_irq_abi and the bench
//! diag array; the register sequences are in internal/etha_life.zig.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const life = @import("internal/etha_life.zig");
const stats = @import("internal/etha_stats.zig");

const tag = "ETHA";
const port_bases = [_]usize{ 0x403C_A000, 0x403C_C000 };
/// `ra8_mstp_t` k_ra8_mstp_eswm: (k_ra8_mstp_reg_c << 8) | 30 (inc/ra8_mstp_regs.h).
const mstp_eswm: u16 = (2 << 8) | 30;
const wait_ms: u32 = 500;
const wait_inner: u32 = 200_000;

/// `ra8_etha_slot_t`.
const Slot = extern struct { cb: ?*const anyopaque = null, ctx: ?*anyopaque = null, stats: stats.Stats = .{} };

/// Per-port handler and counter slots; etha_stats_abi and etha_irq_abi
/// reach them by this name.
export var s_etha_slots: [port_bases.len]Slot = .{ .{}, .{} };
/// Last EAMS.OPS seen by the mode poll, per port. Bench-only (JLink memprobe).
export var g_ra8_etha_diag_last_eams: [port_bases.len]u32 = .{ 0, 0 };

extern fn ra8_mstp_enable(id: u16) u16;

/// Host C tests arm failures through this seam, as the C wait did under
/// UNIT_TEST. Freestanding builds never see it.
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

comptime {
    if (@sizeOf(life.Config) != 16) @compileError("ra8_etha_config_t is 16 bytes");
    if (@sizeOf(Slot) != 2 * @sizeOf(usize) + 28) @compileError("ra8_etha_slot_t layout");
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

fn portOk(p: u8) bool {
    return p < port_bases.len;
}

export fn ra8_etha_init(p: u8, cfg: ?*const life.Config) u16 {
    const c = cfg orelse return fail(common.k_ra8_err_null_ptr, "etha_init: cfg must not be nullptr");
    if (!portOk(p)) return fail(common.k_ra8_err_invalid_arg, "etha_init: port out of range");
    const err = ra8_mstp_enable(mstp_eswm);
    if (err != common.k_ra8_ok) {
        common.ra8_log_emit_error(tag, "etha_init: mstp enable");
        common.ra8_log_emit_error_val(tag, "Error", err);
        return err;
    }
    life.init(regs(p), c);
    s_etha_slots[p] = .{};
    common.ra8_log_emit_info(tag, "etha_init");
    return common.k_ra8_ok;
}

export fn ra8_etha_deinit(p: u8) u16 {
    if (!portOk(p)) return fail(common.k_ra8_err_invalid_arg, "etha_deinit: port out of range");
    life.deinit(regs(p));
    s_etha_slots[p] = .{};
    return common.k_ra8_ok;
}

export fn ra8_etha_enter_stop(p: u8) u16 {
    if (!portOk(p)) return fail(common.k_ra8_err_invalid_arg, "etha_enter_stop: port out of range");
    regs(p).write32(life.off_eamc, life.opc_disable);
    return common.k_ra8_ok;
}

export fn ra8_etha_exit_stop(p: u8) u16 {
    if (!portOk(p)) return fail(common.k_ra8_err_invalid_arg, "etha_exit_stop: port out of range");
    regs(p).write32(life.off_eamc, life.opc_operation);
    return common.k_ra8_ok;
}

export fn ra8_etha_reset(p: u8) u16 {
    if (!portOk(p)) return fail(common.k_ra8_err_invalid_arg, "etha_reset: port out of range");
    life.reset(regs(p));
    return common.k_ra8_ok;
}

export fn ra8_etha_set_mode(p: u8, mode: u8) u16 {
    if (!portOk(p) or mode > life.mask_opc) return fail(common.k_ra8_err_invalid_arg, "etha_set_mode: bad arg");
    regs(p).write32(life.off_eamc, mode & life.mask_opc);
    if (!life.waits(mode)) return common.k_ra8_ok;
    return waitMode(p, mode & life.mask_ops);
}

fn waitMode(p: u8, target: u32) u16 {
    const eams: *volatile u32 = @ptrFromInt(port_bases[p] + life.off_eams);
    var ms: u32 = 0;
    while (ms < wait_ms) : (ms += 1) {
        var i: u32 = 0;
        while (i < wait_inner) : (i += 1) {
            const ops = eams.* & life.mask_ops;
            g_ra8_etha_diag_last_eams[p & 1] = ops;
            const cond = ops == target;
            if (if (hosted) seam.ra8_fake_mmio_wait_eval(eams, i, cond) else cond) return common.k_ra8_ok;
        }
    }
    return fail(common.k_ra8_err_hw_timeout, "etha_set_mode: EAMS.OPS never reached requested mode");
}
