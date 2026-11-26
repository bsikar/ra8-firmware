//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ULPT0-backed periodic deep idle in `ra8_lpm.h` (RA8FW-249).
//! Compiled as its own archive member (`lpm_ulpt`) so images that do not use
//! ULPT self-wake never pull ULPT or ISR symbols into their links.

const common = @import("abi_common.zig");
const impl = @import("internal/lpm_ulpt.zig");

const IsrHandler = ?*const fn (?*anyopaque) callconv(.c) void;

extern fn ra8_ulpt_init() u16;
extern fn ra8_ulpt_start(channel: u8, period: u32) u16;
extern fn ra8_ulpt_stop(channel: u8) u16;
extern fn ra8_ulpt_get_status(channel: u8, out_mask: ?*u8) u16;

extern fn ra8_lpm_set_clock_stop(clock: u8, stop: bool) u16;
extern fn ra8_lpm_arm_wupen1_bits(bits: u32) u16;
extern fn ra8_lpm_clear_wupen1_bits(bits: u32) u16;
extern fn ra8_lpm_prcr_unlock() u16;
extern fn ra8_lpm_prcr_relock() u16;
extern fn ra8_lpm_enter_sleep(mode: u8) u16;

extern fn ra8_isr_register(event: u16, handler: IsrHandler, ctx: ?*anyopaque, priority: u8, out_slot: ?*u16) u16;
extern fn ra8_isr_unregister(event: u16) u16;
extern fn ra8_isr_lookup_slot(event: u16, out_slot: ?*u16) u16;

fn defaultWakeIsr(ctx: ?*anyopaque) callconv(.c) void {
    _ = ctx;
}

const PrepareOps = struct {
    cb: IsrHandler,
    ctx: ?*anyopaque,

    pub fn ulptInit(_: PrepareOps) u16 {
        return ra8_ulpt_init();
    }
    pub fn keepLoco(_: PrepareOps, stop: bool) u16 {
        return ra8_lpm_set_clock_stop(impl.clock_loco, stop);
    }
    pub fn isrLookup(_: PrepareOps, event: u16, out_slot: *u16) u16 {
        return ra8_isr_lookup_slot(event, out_slot);
    }
    pub fn isrUnregister(_: PrepareOps, event: u16) u16 {
        return ra8_isr_unregister(event);
    }
    pub fn isrRegister(self: PrepareOps, event: u16, prio: u8) u16 {
        var slot: u16 = impl.isr_slot_none;
        const active_cb = self.cb orelse defaultWakeIsr;
        return ra8_isr_register(event, active_cb, self.ctx, prio, &slot);
    }
    pub fn armWupen1(_: PrepareOps, bits: u32) u16 {
        return ra8_lpm_arm_wupen1_bits(bits);
    }
};

const StandbyOps = struct {
    pub fn ulptStart(_: StandbyOps, ch: u8, period: u32) u16 {
        return ra8_ulpt_start(ch, period);
    }
    pub fn ulptStop(_: StandbyOps, ch: u8) u16 {
        return ra8_ulpt_stop(ch);
    }
    pub fn ulptGetStatus(_: StandbyOps, ch: u8, out: *u8) u16 {
        return ra8_ulpt_get_status(ch, out);
    }
    pub fn waitEval(_: StandbyOps, iter: u32, cond: bool) bool {
        const reg: *const volatile u8 = @ptrFromInt(impl.ulpt0_cr_addr);
        return common.waitEval(reg, iter, cond);
    }
    pub fn prcrUnlock(_: StandbyOps) u16 {
        return ra8_lpm_prcr_unlock();
    }
    pub fn prcrRelock(_: StandbyOps) u16 {
        return ra8_lpm_prcr_relock();
    }
    pub fn enterSleep(_: StandbyOps, mode: u8) u16 {
        return ra8_lpm_enter_sleep(mode);
    }
};

const DisarmOps = struct {
    pub fn ulptStop(_: DisarmOps, ch: u8) u16 {
        return ra8_ulpt_stop(ch);
    }
    pub fn clearWupen1(_: DisarmOps, bits: u32) u16 {
        return ra8_lpm_clear_wupen1_bits(bits);
    }
    pub fn isrLookup(_: DisarmOps, event: u16, out_slot: *u16) u16 {
        return ra8_isr_lookup_slot(event, out_slot);
    }
    pub fn isrUnregister(_: DisarmOps, event: u16) u16 {
        return ra8_isr_unregister(event);
    }
};

export fn ra8_lpm_ulpt_ms_to_ticks(period_ms: u32, out_ticks: ?*u32) u16 {
    return impl.msToTicks(period_ms, out_ticks);
}

export fn ra8_lpm_ulpt_wake_prepare(isr_cb: IsrHandler, isr_ctx: ?*anyopaque, priority: u8) u16 {
    return impl.prepare(PrepareOps{ .cb = isr_cb, .ctx = isr_ctx }, priority);
}

export fn ra8_lpm_ulpt_standby_ticks(period_ticks: u32) u16 {
    return impl.standbyTicks(StandbyOps{}, period_ticks);
}

export fn ra8_lpm_ulpt_standby_ms(period_ms: u32) u16 {
    return impl.standbyMs(StandbyOps{}, period_ms);
}

export fn ra8_lpm_ulpt_wake_disarm() u16 {
    return impl.disarm(DisarmOps{});
}
