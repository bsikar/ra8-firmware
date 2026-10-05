//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_pdg.h (RA8FW-799), replacing ra8_pdg.c. The logic is in
//! internal/pdg.zig.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const pdg = @import("internal/pdg.zig");

const tag = "PDG";
const freestanding = builtin.os.tag == .freestanding;
const ok = pdg.codes.ok;
const invalid_arg = pdg.codes.invalid_arg;
const not_initialized = pdg.codes.not_initialized;

const EventFn = *const fn (ctx: ?*anyopaque) callconv(.C) void;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn ra8_hw_nop() void;

var event_fn: ?EventFn = null;
var event_ctx: ?*anyopaque = null;

fn nop() void {
    if (freestanding) {
        asm volatile ("nop");
    } else {
        ra8_hw_nop();
    }
}

const Hw = struct {
    fn at(comptime T: type, a: usize) *volatile T {
        return @ptrFromInt(a);
    }
    pub fn read16(_: Hw, a: usize) u16 {
        return at(u16, a).*;
    }
    pub fn write16(_: Hw, a: usize, v: u16) void {
        at(u16, a).* = v;
    }
    pub fn waitUs(_: Hw, us: u16) void {
        var u: u16 = 0;
        while (u < us) : (u += 1) {
            var i: u16 = 0;
            while (i < pdg.loops_per_us) : (i += 1) nop();
        }
    }
    pub fn wait5Gtclk(_: Hw) void {
        var i: u16 = 0;
        while (i < pdg.post_reset_loops) : (i += 1) nop();
    }
};

const hw = Hw{};

/// RA8_RETURN_ON_ERROR: the message, then the code.
fn failed(rc: u16, msg: [*:0]const u8) bool {
    if (rc == ok) return false;
    common.ra8_log_emit_error(tag, msg);
    common.ra8_log_emit_error_val(tag, "Error", rc);
    return true;
}

fn nullFail(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return pdg.codes.null_ptr;
}

fn initialized() bool {
    return pdg.isInitialized(hw.read16(pdg.gtdlycr));
}

export fn ra8_pdg_init(cfg: ?*const pdg.Config) u16 {
    const c = cfg orelse return nullFail("cfg must not be nullptr");
    const rc = pdg.validateCfg(c.*);
    if (failed(rc, "pdg_init: cfg invalid")) return rc;
    var frange = c.frange;
    if (c.auto_tune != 0) {
        const p = pdg.pickFrange(c.gptclk_hz);
        if (failed(p.rc, "pdg_init: auto-tune failed")) return p.rc;
        frange = p.frange;
    }
    const mst = ra8_mstp_enable(pdg.mstp_pdg);
    if (failed(mst, "pdg_init: mstp enable")) return mst;
    pdg.programDll(hw, c.channel_mask, frange);
    common.ra8_log_emit_info(tag, "pdg_init");
    return ok;
}

export fn ra8_pdg_deinit() u16 {
    pdg.park(hw);
    _ = ra8_mstp_disable(pdg.mstp_pdg);
    return ok;
}

export fn ra8_pdg_set_delay(channel: u8, pin: u8, edge: u8, code: u8) u16 {
    if (!initialized()) return not_initialized;
    const v = pdg.slotOk(channel, pin, edge, code);
    if (failed(v, "set_delay: bad input")) return v;
    hw.write16(pdg.cellAddr(channel, pin, edge), code & pdg.dly_mask);
    return ok;
}

export fn ra8_pdg_get_delay(channel: u8, pin: u8, edge: u8, out_code: ?*u8) u16 {
    const out = out_code orelse return nullFail("out_code must not be nullptr");
    const v = pdg.slotOk(channel, pin, edge, 0);
    if (failed(v, "get_delay: bad input")) return v;
    out.* = @intCast(hw.read16(pdg.cellAddr(channel, pin, edge)) & pdg.dly_mask);
    return ok;
}

export fn ra8_pdg_set_delay_batch(entries: ?[*]const pdg.DelayEntry, count: u8) u16 {
    const e = entries orelse return nullFail("entries must not be nullptr");
    if (!initialized()) return not_initialized;
    if (count == 0 or count > pdg.slot_count) return invalid_arg;
    const list = e[0..count];
    for (list) |x| {
        const v = pdg.slotOk(x.channel, x.pin, x.edge, x.code);
        if (v != ok) return v;
    }
    for (list) |x| hw.write16(pdg.cellAddr(x.channel, x.pin, x.edge), x.code & pdg.dly_mask);
    return ok;
}

export fn ra8_pdg_delay_ns_to_code(delay_ns: u32, gptclk_hz: u32, frange: u8, out_code: ?*u8) u16 {
    const out = out_code orelse return nullFail("out_code must not be nullptr");
    if (gptclk_hz == 0 or !pdg.frangeOk(frange)) return invalid_arg;
    out.* = pdg.nsToCode(delay_ns, gptclk_hz, frange);
    return ok;
}

export fn ra8_pdg_exit_stop(channel: u8) u16 {
    if (channel >= pdg.channel_count) return invalid_arg;
    pdg.setCr2Bit(hw, pdg.dlyenBit(channel), false);
    return ok;
}

export fn ra8_pdg_enter_stop(channel: u8) u16 {
    if (channel >= pdg.channel_count) return invalid_arg;
    pdg.setCr2Bit(hw, pdg.dlyenBit(channel), true);
    return ok;
}

export fn ra8_pdg_channel_bypass_set(channel: u8, bypass: u8) u16 {
    if (!initialized()) return not_initialized;
    if (channel >= pdg.channel_count) return invalid_arg;
    pdg.setCr2Bit(hw, pdg.dlybsBit(channel), bypass != 0);
    return ok;
}

export fn ra8_pdg_pin_disable(channel: u8, pin: u8) u16 {
    if (!initialized()) return not_initialized;
    if (channel >= pdg.channel_count) return invalid_arg;
    if (pin != pdg.pin_a and pin != pdg.pin_b) return invalid_arg;
    hw.write16(pdg.cellAddr(channel, pin, pdg.edge_rising), 0);
    hw.write16(pdg.cellAddr(channel, pin, pdg.edge_falling), 0);
    return ok;
}

export fn ra8_pdg_get_status(out: ?*u16) u16 {
    const o = out orelse return nullFail("out must not be nullptr");
    o.* = hw.read16(pdg.gtdlycr);
    return ok;
}

export fn ra8_pdg_get_status_full(out: ?*pdg.StatusFull) u16 {
    const o = out orelse return nullFail("out must not be nullptr");
    const cr = hw.read16(pdg.gtdlycr);
    o.* = pdg.decodeStatus(cr, hw.read16(pdg.gtdlycr2));
    return ok;
}

export fn ra8_pdg_clear_status(mask: u16) u16 {
    if (mask & pdg.status_in_reset != 0) {
        hw.write16(pdg.gtdlycr, hw.read16(pdg.gtdlycr) & ~pdg.dlyrst);
    }
    return ok;
}

export fn ra8_pdg_attach_handler(f: ?EventFn, ctx: ?*anyopaque) u16 {
    event_fn = f;
    event_ctx = ctx;
    return ok;
}

export fn ra8_pdg_dispatch() void {
    const f = event_fn;
    const ctx = event_ctx;
    if (f) |cb| cb(ctx);
}

export fn ra8_pdg_capture_start(buf: ?[*]const pdg.DelayEntry, len: u8) u16 {
    const b = buf orelse return nullFail("buf must not be nullptr");
    if (len == 0) return invalid_arg;
    const rc = ra8_pdg_set_delay_batch(b, len);
    if (rc != ok) return rc;
    ra8_pdg_dispatch();
    return ok;
}

export fn ra8_pdg_capture_stop() u16 {
    return ok;
}

export fn ra8_pdg_pick_frange(gptclk_hz: u32, out: ?*u8) u16 {
    const o = out orelse return nullFail("out must not be nullptr");
    const p = pdg.pickFrange(gptclk_hz);
    if (p.rc == ok) o.* = p.frange;
    return p.rc;
}

export fn ra8_pdg_set_frange(new_frange: u8) u16 {
    if (!initialized()) return not_initialized;
    if (!pdg.frangeOk(new_frange)) return invalid_arg;
    pdg.switchFrange(hw, new_frange);
    return ok;
}

fn gtwp(channel: u8) *volatile u32 {
    return @ptrFromInt(pdg.gpt0_base + @as(usize, channel) * pdg.gpt_stride);
}

/// Write GTWP, then read it back so the write lands before returning.
fn writeGtwp(channel: u8, key: u32) void {
    const p = gtwp(channel);
    p.* = key;
    _ = p.*;
}

export fn ra8_pdg_bind_gpt_channel(channel: u8) u16 {
    if (!initialized()) return not_initialized;
    if (channel >= pdg.channel_count) return invalid_arg;
    writeGtwp(channel, pdg.gtwp_unlock);
    return ok;
}

export fn ra8_pdg_unbind_gpt_channel(channel: u8) u16 {
    if (channel >= pdg.channel_count) return invalid_arg;
    writeGtwp(channel, pdg.gtwp_lock);
    return ok;
}

export fn ra8_pdg_check_constraints(mode: u8, dir: u8, compare_match: u32, gtpr: u32) u16 {
    return pdg.checkConstraints(mode, dir, compare_match, gtpr);
}

export fn ra8_pdg_required_write_ns(pclka_hz: u32, gptclk_hz: u32, out_ns: ?*u32) u16 {
    const o = out_ns orelse return nullFail("out_ns must not be nullptr");
    if (pclka_hz == 0 or gptclk_hz == 0) return invalid_arg;
    o.* = pdg.requiredWriteNs(pclka_hz, gptclk_hz);
    return ok;
}
