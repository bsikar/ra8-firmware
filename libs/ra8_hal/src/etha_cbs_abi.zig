//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ETHA CBS and error-counter entry points (RA8FW-816). Logic
//! is in internal/etha_cbs.zig; checks run in the order the C used.

const common = @import("abi_common.zig");
const cbs = @import("internal/etha_cbs.zig");

const tag = "ETHA";
const port_bases = [_]usize{ 0x403C_A000, 0x403C_C000 };

comptime {
    if (@sizeOf(cbs.Param) != 8) @compileError("ra8_etha_cbs_param_t is 8 bytes");
    if (@sizeOf(cbs.Counters) != 10) @compileError("ra8_etha_stats_t is 10 bytes");
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

export fn ra8_etha_configure_cbs(p: u8, tc: u8, enable: u8, param: ?*const cbs.Param) u16 {
    if (p >= port_bases.len or !cbs.tcOk(tc)) return fail(common.k_ra8_err_invalid_arg, "etha_configure_cbs: bad arg");
    if (enable == 0) {
        cbs.configure(regs(p), tc, null);
        return common.k_ra8_ok;
    }
    const pr = param orelse return fail(common.k_ra8_err_null_ptr, "etha_configure_cbs: param null");
    if (!cbs.paramOk(pr)) return fail(common.k_ra8_err_invalid_arg, "etha_configure_cbs: param range");
    cbs.configure(regs(p), tc, pr);
    return common.k_ra8_ok;
}

export fn ra8_etha_get_cbs_state(p: u8, tc: u8, enabled: ?*u8, gate_open: ?*u8, oper_param: ?*cbs.Param) u16 {
    const en = enabled orelse return fail(common.k_ra8_err_null_ptr, "etha_get_cbs_state: enabled null");
    const gate = gate_open orelse return fail(common.k_ra8_err_null_ptr, "etha_get_cbs_state: gate_open null");
    const oper = oper_param orelse return fail(common.k_ra8_err_null_ptr, "etha_get_cbs_state: oper_param null");
    if (p >= port_bases.len or !cbs.tcOk(tc)) return fail(common.k_ra8_err_invalid_arg, "etha_get_cbs_state: bad arg");
    const s = cbs.state(regs(p), tc);
    en.* = s.enabled;
    gate.* = s.gate_open;
    oper.* = s.oper;
    return common.k_ra8_ok;
}

export fn ra8_etha_read_stats(p: u8, out: ?*cbs.Counters) u16 {
    const o = out orelse return fail(common.k_ra8_err_null_ptr, "etha_read_stats: out null");
    if (p >= port_bases.len) return fail(common.k_ra8_err_invalid_arg, "etha_read_stats: port out of range");
    o.* = cbs.readCounters(regs(p));
    return common.k_ra8_ok;
}

export fn ra8_etha_clear_stats(p: u8) u16 {
    if (p >= port_bases.len) return fail(common.k_ra8_err_invalid_arg, "etha_clear_stats: port out of range");
    cbs.clearCounters(regs(p));
    return common.k_ra8_ok;
}
