//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_agt.h (RA8FW-800), replacing ra8_agt.c. The logic is in
//! internal/agt.zig.

const common = @import("abi_common.zig");
const agt = @import("internal/agt.zig");

const tag = "AGT";
const ok = agt.codes.ok;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

var held: [agt.mstp_count]bool = @splat(false);
var event_fn: ?agt.EventFn = null;
var event_ctx: ?*anyopaque = null;

const Hw = struct {
    pub fn read8(_: Hw, a: usize) u8 {
        return @as(*volatile u8, @ptrFromInt(a)).*;
    }
    pub fn write8(_: Hw, a: usize, v: u8) void {
        @as(*volatile u8, @ptrFromInt(a)).* = v;
    }
    pub fn write16(_: Hw, a: usize, v: u16) void {
        @as(*volatile u16, @ptrFromInt(a)).* = v;
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
    return agt.codes.null_ptr;
}

/// Take the channel's module-stop reference once (AGT0/AGT1 only).
fn acquire(channel: u8) u16 {
    if (channel >= agt.mstp_count or held[channel]) return ok;
    const rc = ra8_mstp_enable(agt.mstp_ids[channel]);
    if (rc != ok) return rc;
    held[channel] = true;
    return ok;
}

fn release(channel: u8) u16 {
    if (channel >= agt.mstp_count or !held[channel]) return ok;
    held[channel] = false;
    return ra8_mstp_disable(agt.mstp_ids[channel]);
}

export fn ra8_agt_start_free_run(channel: u8, reload: u16) u16 {
    const r = agt.regs(channel) orelse return nullFail("channel out of range");
    const rc = acquire(channel);
    if (failed(rc, "agt_start: mstp enable")) return rc;
    agt.startFreeRun(hw, r, reload);
    common.ra8_log_emit_info_val(tag, "start channel", channel);
    return ok;
}

export fn ra8_agt_stop(channel: u8) u16 {
    const r = agt.regs(channel) orelse return nullFail("channel out of range");
    hw.write8(r + agt.off_cr, 0);
    return ok;
}

fn stopAndRelease(channel: u8) u16 {
    const r = agt.regs(channel) orelse return nullFail("channel out of range");
    hw.write8(r + agt.off_cr, 0);
    return release(channel);
}

export fn ra8_agt_deinit(channel: u8) u16 {
    return stopAndRelease(channel);
}

export fn ra8_agt_set_reload(channel: u8, reload: u16) u16 {
    const r = agt.regs(channel) orelse return nullFail("channel out of range");
    hw.write16(r + agt.off_agt, reload);
    return ok;
}

export fn ra8_agt_get_status(channel: u8, out_mask: ?*u8) u16 {
    const out = out_mask orelse return nullFail("out_mask must not be nullptr");
    const r = agt.regs(channel) orelse return nullFail("channel out of range");
    out.* = hw.read8(r + agt.off_cr);
    return ok;
}

export fn ra8_agt_attach_handler(f: ?agt.EventFn, ctx: ?*anyopaque) u16 {
    event_fn = f;
    event_ctx = ctx;
    return ok;
}

export fn ra8_agt_dispatch(channel: u8) void {
    if (agt.regs(channel) == null) return;
    const f = event_fn;
    const ctx = event_ctx;
    if (f) |cb| cb(ctx, channel);
}

export fn ra8_agt_enter_stop(channel: u8) u16 {
    return stopAndRelease(channel);
}

export fn ra8_agt_exit_stop(channel: u8) u16 {
    if (agt.regs(channel) == null) return agt.codes.invalid_arg;
    return acquire(channel);
}

export fn ra8_agt_start_pulse_output(channel: u8, cfg: ?*const agt.PulseCfg) u16 {
    const c = cfg orelse return nullFail("cfg must not be nullptr");
    const r = agt.regs(channel) orelse return nullFail("channel out of range");
    const v: u16 = if (agt.pulseCfgOk(c.*)) ok else agt.codes.invalid_arg;
    if (failed(v, "agt_pulse: cfg validation")) return v;
    const rc = acquire(channel);
    if (failed(rc, "agt_pulse: mstp enable")) return rc;
    agt.programPulse(hw, r, c.*);
    hw.write8(r + agt.off_cr, agt.cr_tstart);
    common.ra8_log_emit_info_val(tag, "pulse start channel", channel);
    return ok;
}

/// Both halves' module-stop references; each failure logs at both levels.
fn acquireCascade() u16 {
    const m0 = acquire(agt.cascade_lo);
    if (failed(m0, "cascade: mstp AGT0")) return m0;
    const m1 = acquire(agt.cascade_hi);
    if (failed(m1, "cascade: mstp AGT1")) return m1;
    return ok;
}

export fn ra8_agt_start_cascade(cfg: ?*const agt.CascadeCfg) u16 {
    const c = cfg orelse return nullFail("cfg must not be nullptr");
    const tck = agt.cascadeTck(c.clock) orelse {
        _ = failed(agt.codes.invalid_arg, "cascade: bad clock enum");
        return agt.codes.invalid_arg;
    };
    const rc = acquireCascade();
    if (failed(rc, "cascade: mstp enable")) return rc;
    agt.armCascade(hw, c.reload32, tck);
    if (c.on_underflow) |f| {
        event_fn = f;
        event_ctx = c.ctx;
    }
    common.ra8_log_emit_info_val(tag, "cascade start", c.reload32);
    return ok;
}
