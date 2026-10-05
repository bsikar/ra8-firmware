//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_cnecc.h (RA8FW-793), replacing ra8_cnecc.c. The logic is in
//! internal/cnecc.zig; this file owns the driver state, the volatile access
//! at ECCMB0/1 (0x4036F200 + 0x100*n) and the MSTP / ISR externs.

const common = @import("abi_common.zig");
const cn = @import("internal/cnecc.zig");

const tag = "CNECC";
const ok = cn.codes.ok;
const invalid_arg = cn.codes.invalid_arg;

const Hw = struct {
    fn reg(comptime T: type, a: usize) *volatile T {
        return @ptrFromInt(a);
    }
    pub fn read16(_: Hw, a: usize) u16 {
        return reg(u16, a).*;
    }
    pub fn write16(_: Hw, a: usize, v: u16) void {
        reg(u16, a).* = v;
    }
    pub fn read32(_: Hw, a: usize) u32 {
        return reg(u32, a).*;
    }
    pub fn write32(_: Hw, a: usize, v: u32) void {
        reg(u32, a).* = v;
    }
};

const hw = Hw{};
const IsrFn = *const fn (ctx: ?*anyopaque) callconv(.C) void;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn ra8_isr_register(event: u16, handler: IsrFn, ctx: ?*anyopaque, priority: u8, out_slot: ?*u16) u16;
extern fn ra8_isr_unregister(event: u16) u16;

var st: cn.State = .{};

fn nullOut(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return cn.codes.null_ptr;
}

/// RA8_RETURN_ON_ERROR: the message, then the code.
fn failed(rc: u16, msg: [*:0]const u8) bool {
    if (rc == ok) return false;
    common.ra8_log_emit_error(tag, msg);
    common.ra8_log_emit_error_val(tag, "Error", rc);
    return true;
}

fn ctlAddr(instance: u8) usize {
    return cn.base(instance) + cn.off.ctl;
}

fn apply(instance: u8, c: cn.InstanceCfg) u16 {
    const rc = ra8_mstp_enable(cn.mstp_ids[instance]);
    if (failed(rc, "cnecc_init: mstp enable")) return rc;
    cn.applyRegs(hw, instance, c);
    st.resetCounts(instance);
    return ok;
}

fn rmw(instance: u8, new_bits: u32, mask: u32) void {
    hw.write32(ctlAddr(instance), cn.ctlRmw(hw.read32(ctlAddr(instance)), new_bits, mask));
}

export fn ra8_cnecc_init(cfg: ?*const cn.Config) u16 {
    const c = cfg orelse return nullOut("cfg must not be nullptr");
    var i: u8 = 0;
    while (i < cn.count) : (i += 1) {
        const rc = apply(i, c.instances[i]);
        if (failed(rc, "cnecc_init apply")) return rc;
    }
    st.cfg = c.*;
    st.initialized = true;
    common.ra8_log_emit_info(tag, "cnecc_init");
    return ok;
}

export fn ra8_cnecc_deinit() u16 {
    if (st.isr_attached) _ = ra8_cnecc_detach_isr();
    var i: u8 = 0;
    while (i < cn.count) : (i += 1) {
        hw.write16(cn.base(i) + cn.off.tmc, cn.tmc.test_disable);
        hw.write32(ctlAddr(i), cn.ctl.emca_unlock);
        _ = ra8_mstp_disable(cn.mstp_ids[i]);
        st.clearCounts(i);
    }
    st.initialized = false;
    return ok;
}

export fn ra8_cnecc_enable_instance(instance: u8) u16 {
    if (instance >= cn.count) return invalid_arg;
    rmw(instance, cn.ctl.ecervf, cn.ctl.ecervf);
    return ok;
}

export fn ra8_cnecc_disable_instance(instance: u8) u16 {
    if (instance >= cn.count) return invalid_arg;
    rmw(instance, 0, cn.ctl.ecervf);
    return ok;
}

export fn ra8_cnecc_enter_standby() u16 {
    var i: u8 = 0;
    while (i < cn.count) : (i += 1) cn.standbyRegs(hw, i);
    return ok;
}

export fn ra8_cnecc_exit_standby() u16 {
    if (!st.initialized) return cn.codes.not_initialized;
    var i: u8 = 0;
    while (i < cn.count) : (i += 1) {
        const rc = apply(i, st.cfg.instances[i]);
        if (failed(rc, "cnecc_exit_standby apply")) return rc;
    }
    return ok;
}

export fn ra8_cnecc_set_irq_enables(instance: u8, irq_1bit: bool, irq_2bit: bool) u16 {
    if (instance >= cn.count) return invalid_arg;
    rmw(instance, cn.irqBits(irq_1bit, irq_2bit), cn.ctl.irq_all);
    st.cfg.instances[instance].irq_1bit = irq_1bit;
    st.cfg.instances[instance].irq_2bit = irq_2bit;
    return ok;
}

export fn ra8_cnecc_set_correction_permission(instance: u8, correct_1bit: bool) u16 {
    if (instance >= cn.count) return invalid_arg;
    rmw(instance, if (correct_1bit) 0 else cn.ctl.ec1ecp, cn.ctl.ec1ecp);
    st.cfg.instances[instance].correct_1bit = correct_1bit;
    return ok;
}

export fn ra8_cnecc_get_status(instance: u8, out: ?*cn.Status) u16 {
    const p = out orelse return nullOut("out must not be nullptr");
    if (instance >= cn.count) return invalid_arg;
    const b = cn.base(instance);
    const c = hw.read32(b + cn.off.ctl);
    const t = hw.read16(b + cn.off.tmc);
    const ead = hw.read32(b + cn.off.ead);
    p.* = cn.decode(c, t, ead, st.counts[instance]);
    return ok;
}

export fn ra8_cnecc_get_counters(instance: u8, out: ?*cn.Counters) u16 {
    const p = out orelse return nullOut("out must not be nullptr");
    if (instance >= cn.count) return invalid_arg;
    p.* = st.counts[instance];
    return ok;
}

export fn ra8_cnecc_reset_counters(instance: u8) u16 {
    if (instance >= cn.count) return invalid_arg;
    st.resetCounts(instance);
    return ok;
}

export fn ra8_cnecc_set_counter_mirror(instance: u8, mirror: ?*cn.Counters) u16 {
    if (instance >= cn.count) return invalid_arg;
    st.setMirror(instance, mirror);
    return ok;
}

export fn ra8_cnecc_clear_status(instance: u8) u16 {
    if (instance >= cn.count) return invalid_arg;
    hw.write32(ctlAddr(instance), cn.ctl.clear_all);
    return ok;
}

export fn ra8_cnecc_inject_fault(instance: u8, req: ?*const cn.Inject) u16 {
    const r = req orelse return nullOut("req must not be nullptr");
    if (instance >= cn.count) return invalid_arg;
    cn.injectRegs(hw, instance, r.substitute);
    return ok;
}

export fn ra8_cnecc_test_mode_disable(instance: u8) u16 {
    if (instance >= cn.count) return invalid_arg;
    hw.write16(cn.base(instance) + cn.off.tmc, cn.tmc.test_disable);
    return ok;
}

export fn ra8_cnecc_test_mode_active(instance: u8, out: ?*bool) u16 {
    const p = out orelse return nullOut("out must not be nullptr");
    if (instance >= cn.count) return invalid_arg;
    p.* = hw.read16(cn.base(instance) + cn.off.tmc) & cn.tmc.ectmce != 0;
    return ok;
}

export fn ra8_cnecc_attach_handler(f: ?cn.ErrorFn, ctx: ?*anyopaque) u16 {
    const h = f orelse return nullOut("fn must not be nullptr");
    st.handler = h;
    st.ctx = ctx;
    return ok;
}

export fn ra8_cnecc_attach_isr(priority: u8) u16 {
    if (priority > cn.prio_max) return invalid_arg;
    var i: u8 = 0;
    while (i < cn.count) : (i += 1) {
        const ctx: ?*anyopaque = @ptrFromInt(i);
        const rc = ra8_isr_register(cn.events[i], &ra8_cnecc_isr_handler, ctx, priority, null);
        if (rc != ok) {
            var j: u8 = 0;
            while (j < i) : (j += 1) _ = ra8_isr_unregister(cn.events[j]);
            return rc;
        }
    }
    st.isr_attached = true;
    return ok;
}

export fn ra8_cnecc_detach_isr() u16 {
    for (cn.events) |e| _ = ra8_isr_unregister(e);
    st.isr_attached = false;
    return ok;
}

export fn ra8_cnecc_isr_handler(ctx: ?*anyopaque) void {
    const instance: u8 = @truncate(@intFromPtr(ctx) & 0xFF);
    if (instance >= cn.count) return;
    const c = hw.read32(ctlAddr(instance));
    const addr: u16 = @truncate(hw.read32(cn.base(instance) + cn.off.ead) & cn.ead_mask);
    if (c & cn.ctl.ecer2f != 0) st.dispatch(instance, true, addr);
    if (c & cn.ctl.ecer1f != 0) st.dispatch(instance, false, addr);
    if (c & cn.ctl.ecovff != 0) st.dispatchOverflow(instance);
    hw.write32(ctlAddr(instance), cn.ctl.clear_all);
}

export fn ra8_cnecc_dispatch(instance: u8, is_2bit: bool, err_addr: u16) void {
    st.dispatch(instance, is_2bit, err_addr);
}

export fn ra8_cnecc_dispatch_overflow(instance: u8) void {
    st.dispatchOverflow(instance);
}

export fn ra8_cnecc_open() u16 {
    const all = cn.InstanceCfg{ .correct_1bit = true, .irq_1bit = true, .irq_2bit = true, .enable = true };
    const cfg = cn.Config{ .instances = .{ all, all } };
    return ra8_cnecc_init(&cfg);
}

export fn ra8_cnecc_compute(addr: u32, len: u32, out_ecc: ?*u32) u16 {
    const out = out_ecc orelse return cn.codes.null_ptr;
    const rc = cn.computeCheck(addr, len);
    if (rc != ok) return rc;
    const p: [*]const u8 = @ptrFromInt(addr);
    out.* = cn.crc32(p[0..cn.alignedLen(len)]);
    common.ra8_log_emit_info_val(tag, "compute ecc", out.*);
    return ok;
}

export fn ra8_cnecc_verify(addr: u32, len: u32, expected_ecc: u32) u16 {
    var got: u32 = 0;
    const rc = ra8_cnecc_compute(addr, len, &got);
    if (failed(rc, "verify: compute failed")) return rc;
    if (got != expected_ecc) {
        common.ra8_log_emit_error_val(tag, "verify mismatch", got);
        return cn.codes.crc_mismatch;
    }
    return ok;
}
