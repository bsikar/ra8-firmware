//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_gpt.h and ra8_gpt_capture.h (RA8FW-801), replacing
//! ra8_gpt.c. The register logic is in internal/gpt.zig; this file keeps
//! the per-channel handler state, the three-phase state and the one-shot
//! GTCLKCR enable.

const common = @import("abi_common.zig");
const gpt = @import("internal/gpt.zig");
const dma = @import("internal/dma.zig");

const tag = "GPT";
const ok = gpt.codes.ok;
const invalid_arg = gpt.codes.invalid_arg;
const invalid_state = gpt.codes.invalid_state;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn ra8_dma_request(req: *const dma.Request, out_channel: *u8) u16;

const State = struct {
    func: ?gpt.EventFn = null,
    ctx: ?*anyopaque = null,
    configured: bool = false,
};

const ThreePhase = struct {
    channels: [gpt.three_phase_count]u8 = .{ 0, 0, 0 },
    mask: u32 = 0,
    open: bool = false,
};

var state = [_]State{.{}} ** gpt.channel_count;
var phase = ThreePhase{};
var clock_on = false;

const Hw = struct {
    pub fn read32(_: Hw, a: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(a)).*;
    }
    pub fn write32(_: Hw, a: usize, v: u32) void {
        @as(*volatile u32, @ptrFromInt(a)).* = v;
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
    return gpt.codes.null_ptr;
}

fn reg(channel: u8) ?usize {
    return gpt.regs(channel);
}

/// GTCLKCR must be programmed before MSTPCR is cleared, or per-channel
/// writes silently drop.
fn clockInit() void {
    if (clock_on) return;
    hw.write32(gpt.gtclkcr, gpt.gtclkcr_bpen);
    clock_on = true;
}

export fn ra8_gpt_start_free_run(channel: u8, period: u32) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    clockInit();
    const rc = ra8_mstp_enable(gpt.mstp_ids[channel]);
    if (failed(rc, "gpt_start: mstp enable")) return rc;
    gpt.startFreeRun(hw, r, channel, period);
    common.ra8_log_emit_info_val(tag, "start channel", channel);
    return ok;
}

export fn ra8_gpt_stop(channel: u8) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    gpt.guarded(hw, r, gpt.off.gtstp, gpt.bit(channel));
    return ok;
}

export fn ra8_gpt_start(channel: u8) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    if (!state[channel].configured) return invalid_state;
    gpt.unlock(hw, r);
    gpt.orIn(hw, r + gpt.off.gtcr, gpt.gtcr_cst);
    hw.write32(r + gpt.off.gtstr, gpt.bit(channel));
    gpt.lock(hw, r);
    return ok;
}

export fn ra8_gpt_read(channel: u8, out: ?*u32) u16 {
    const o = out orelse return nullFail("out must not be nullptr");
    const r = reg(channel) orelse return nullFail("channel out of range");
    o.* = hw.read32(r + gpt.off.gtcnt);
    return ok;
}

export fn ra8_gpt_init(channel: u8, cfg: ?*const gpt.Cfg) u16 {
    const c = cfg orelse return nullFail("cfg must not be nullptr");
    const r = reg(channel) orelse return nullFail("channel out of range");
    clockInit();
    const rc = ra8_mstp_enable(gpt.mstp_ids[channel]);
    if (failed(rc, "gpt_init: mstp enable")) return rc;
    gpt.initRegs(hw, r, channel, c);
    state[channel].configured = true;
    common.ra8_log_emit_info_val(tag, "init channel", channel);
    return ok;
}

export fn ra8_gpt_deinit(channel: u8) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    gpt.unlock(hw, r);
    hw.write32(r + gpt.off.gtstp, gpt.bit(channel));
    hw.write32(r + gpt.off.gtcr, 0);
    gpt.lock(hw, r);
    state[channel] = .{};
    _ = ra8_mstp_disable(gpt.mstp_ids[channel]);
    return ok;
}

export fn ra8_gpt_set_period(channel: u8, period: u32) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    gpt.unlock(hw, r);
    hw.write32(r + gpt.off.gtpr, period);
    hw.write32(r + gpt.off.gtpbr, period);
    gpt.lock(hw, r);
    return ok;
}

export fn ra8_gpt_set_duty(channel: u8, which: u8, value: u32) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    if (which > gpt.ccr_b) return invalid_arg;
    gpt.guarded(hw, r, gpt.ccr(which), value);
    return ok;
}

export fn ra8_gpt_get_status(channel: u8, out_mask: ?*u32) u16 {
    const o = out_mask orelse return nullFail("out_mask must not be nullptr");
    const r = reg(channel) orelse return nullFail("channel out of range");
    o.* = hw.read32(r + gpt.off.gtst) & gpt.gtst_mask;
    return ok;
}

export fn ra8_gpt_clear_status(channel: u8, mask: u32) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    gpt.clearFlags(hw, r, mask & gpt.gtst_mask);
    return ok;
}

export fn ra8_gpt_attach_handler(channel: u8, func: ?gpt.EventFn, ctx: ?*anyopaque) u16 {
    if (channel >= gpt.channel_count) return invalid_arg;
    state[channel].func = func;
    state[channel].ctx = ctx;
    return ok;
}

export fn ra8_gpt_enter_stop(channel: u8) u16 {
    const r = reg(channel) orelse return invalid_arg;
    gpt.guarded(hw, r, gpt.off.gtstp, gpt.bit(channel));
    return ra8_mstp_disable(gpt.mstp_ids[channel]);
}

export fn ra8_gpt_exit_stop(channel: u8) u16 {
    if (channel >= gpt.channel_count) return invalid_arg;
    return ra8_mstp_enable(gpt.mstp_ids[channel]);
}

/// Word-wide DMA between memory and one fixed GPT register.
fn dmaStream(src: usize, dst: usize, count: u16, to_reg: bool, done: ?dma.CompleteFn, ctx: ?*anyopaque, out: *u8) u16 {
    const req = dma.Request{
        .src_addr = src,
        .dst_addr = dst,
        .count = count,
        .width = dma.width_word,
        .src_inc = to_reg,
        .dst_inc = !to_reg,
        .trigger = 0,
        .on_complete = done,
        .ctx = ctx,
    };
    return ra8_dma_request(&req, out);
}

export fn ra8_gpt_write_dma(channel: u8, periods: ?[*]const u32, count: u16, done: ?dma.CompleteFn, ctx: ?*anyopaque, out_dma_channel: ?*u8) u16 {
    const p = periods orelse return nullFail("gpt_write_dma: periods");
    const o = out_dma_channel orelse return nullFail("gpt_write_dma: out_dma_channel");
    const r = reg(channel) orelse return invalid_arg;
    if (count == 0) return invalid_arg;
    return dmaStream(@intFromPtr(p), r + gpt.off.gtpr, count, true, done, ctx, o);
}

export fn ra8_gpt_read_dma(channel: u8, out_counts: ?[*]u32, count: u16, done: ?dma.CompleteFn, ctx: ?*anyopaque, out_dma_channel: ?*u8) u16 {
    const p = out_counts orelse return nullFail("gpt_read_dma: out_counts");
    const o = out_dma_channel orelse return nullFail("gpt_read_dma: out_dma_channel");
    const r = reg(channel) orelse return invalid_arg;
    if (count == 0) return invalid_arg;
    return dmaStream(r + gpt.off.gtcnt, @intFromPtr(p), count, false, done, ctx, o);
}

export fn ra8_gpt_capture_configure(channel: u8, which: u8, source_mask: u32) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    if (which > gpt.ccr_b or source_mask & ~gpt.cap_src_valid != 0) return invalid_arg;
    const o = if (which == gpt.ccr_a) gpt.off.gticasr else gpt.off.gticbsr;
    gpt.guarded(hw, r, o, source_mask);
    return ok;
}

export fn ra8_gpt_capture_read(channel: u8, which: u8, out_value: ?*u32) u16 {
    const o = out_value orelse return nullFail("out_value must not be nullptr");
    const r = reg(channel) orelse return nullFail("channel out of range");
    if (which > gpt.ccr_b) return invalid_arg;
    o.* = hw.read32(r + gpt.ccr(which));
    return ok;
}

export fn ra8_gpt_event_count_configure(channel: u8, up_source: u32, down_source: u32) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    if (up_source & ~gpt.cnt_src_valid != 0 or down_source & ~gpt.cnt_src_valid != 0) return invalid_arg;
    gpt.unlock(hw, r);
    hw.write32(r + gpt.off.gtupsr, up_source);
    hw.write32(r + gpt.off.gtdnsr, down_source);
    gpt.lock(hw, r);
    return ok;
}

export fn ra8_gpt_period_set(channel: u8, period_counts: u32) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    gpt.periodSet(hw, r, period_counts);
    return ok;
}

export fn ra8_gpt_duty_cycle_set(channel: u8, pin: u8, compare_counts: u32) u16 {
    if (pin > 1) return invalid_arg;
    const r = reg(channel) orelse return nullFail("channel out of range");
    gpt.bufferedDuty(hw, r, pin == 1, compare_counts);
    return ok;
}

export fn ra8_gpt_counter_set(channel: u8, value: u32) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    if (hw.read32(r + gpt.off.gtcr) & gpt.gtcr_cst != 0) return invalid_state;
    gpt.guarded(hw, r, gpt.off.gtcnt, value);
    return ok;
}

export fn ra8_gpt_pwm_pin_configure(channel: u8, pin: u8, cfg: ?*const gpt.PinCfg) u16 {
    const c = cfg orelse return nullFail("cfg must not be nullptr");
    if (pin > 1) return invalid_arg;
    const r = reg(channel) orelse return nullFail("channel out of range");
    gpt.unlock(hw, r);
    const v = gpt.packGtior(hw.read32(r + gpt.off.gtior), pin == 1, c.*);
    hw.write32(r + gpt.off.gtior, v);
    gpt.lock(hw, r);
    return ok;
}

export fn ra8_gpt_dead_time_set(channel: u8, rising_dt: u32, falling_dt: u32) u16 {
    const r = reg(channel) orelse return nullFail("channel out of range");
    gpt.deadTime(hw, r, rising_dt, falling_dt);
    return ok;
}

/// Init each phase channel; on failure deinit the ones already opened.
fn openPhases(cfg: *const gpt.ThreePhaseCfg) u16 {
    const duties = [_]u32{ cfg.initial_duty_u, cfg.initial_duty_v, cfg.initial_duty_w };
    var mask: u32 = 0;
    for (cfg.channels, duties, 0..) |ch, duty, i| {
        const per = gpt.Cfg{ .mode = cfg.mode, .prescaler = cfg.prescaler, .period = cfg.period_counts, .duty_a = duty, .duty_b = duty, .auto_start = false };
        const rc = ra8_gpt_init(ch, &per);
        if (rc != ok) {
            for (cfg.channels[0..i]) |prev| _ = ra8_gpt_deinit(prev);
            return rc;
        }
        mask |= gpt.bit(ch);
        phase.channels[i] = ch;
    }
    phase.mask = mask;
    return ok;
}

export fn ra8_gpt_three_phase_open(cfg: ?*const gpt.ThreePhaseCfg) u16 {
    const c = cfg orelse return nullFail("three_phase cfg must not be nullptr");
    if (phase.open) return invalid_state;
    for (c.channels) |ch| if (ch >= gpt.channel_count) return invalid_arg;
    const rc = openPhases(c);
    if (rc != ok) return rc;
    // One GTSTR write on the U channel starts all three on the same edge.
    gpt.guarded(hw, gpt.regs(c.channels[0]).?, gpt.off.gtstr, phase.mask);
    phase.open = true;
    common.ra8_log_emit_info_val(tag, "three_phase open mask", phase.mask);
    return ok;
}

export fn ra8_gpt_three_phase_set_duty(u_duty: u32, v_duty: u32, w_duty: u32) u16 {
    if (!phase.open) return invalid_state;
    const duties = [_]u32{ u_duty, v_duty, w_duty };
    const period = hw.read32(gpt.regs(phase.channels[0]).? + gpt.off.gtpr);
    for (duties) |d| if (d > period) return invalid_arg;
    for (phase.channels, duties) |ch, d| gpt.phaseDuty(hw, gpt.regs(ch).?, d);
    return ok;
}

export fn ra8_gpt_three_phase_close() u16 {
    if (!phase.open) return invalid_state;
    gpt.guarded(hw, gpt.regs(phase.channels[0]).?, gpt.off.gtstp, phase.mask);
    for (phase.channels) |ch| _ = ra8_gpt_deinit(ch);
    phase.mask = 0;
    phase.open = false;
    return ok;
}

fn dispatch(channel: u8, status_mask: u32) void {
    const r = reg(channel) orelse return;
    gpt.clearFlags(hw, r, status_mask);
    const s = state[channel];
    if (s.func) |f| f(s.ctx, status_mask);
}

export fn ra8_gpt_dispatch_ovf(channel: u8) void {
    dispatch(channel, gpt.status_overflow);
}

export fn ra8_gpt_dispatch_und(channel: u8) void {
    dispatch(channel, gpt.status_underflow);
}

export fn ra8_gpt_dispatch_ccra(channel: u8) void {
    dispatch(channel, gpt.status_ccra);
}

export fn ra8_gpt_dispatch_ccrb(channel: u8) void {
    dispatch(channel, gpt.status_ccrb);
}
