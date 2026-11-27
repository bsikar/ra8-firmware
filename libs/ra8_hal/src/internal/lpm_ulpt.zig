//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure ULPT0 deep-idle (Software Standby self-wake) sequence (RA8FW-249).
//! Parameterized over `ops` so native Zig tests verify every edge path without
//! real MMIO or C globals, while `lpm_ulpt_abi.zig` binds it to `ra8_ulpt`,
//! `ra8_lpm`, `ra8_isr`, and `ra8_fake_mmio_wait_eval`.

const std = @import("std");

pub const ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_not_found: u16 = 0x106;
pub const err_hw_timeout: u16 = 0x203;
pub const err_null_ptr: u16 = 0x504;

pub const channel: u8 = 0;
pub const clock_loco: u8 = 2;
pub const mode_software_std: u8 = 0x5;
pub const elc_event_ulpt0_ulpti: u16 = 0x080;
pub const wupen1_ulpt0u: u32 = 1 << 8;
pub const tcstf_mask: u8 = 0x02;
pub const ulpt0_cr_addr: usize = 0x4022000C;
pub const start_poll_max: u32 = 0x40000;
pub const isr_slot_none: u16 = 0xFFFF;
pub const isr_prio_max: u8 = 15;
pub const loco_hz: u64 = 32768;
pub const ms_per_sec: u64 = 1000;

/// Round-up ms -> 32-bit ULPT0 LOCO ticks, rejecting zero and u32 overflow.
pub fn msToTicks(period_ms: u32, out_ticks: ?*u32) u16 {
    const out = out_ticks orelse return err_null_ptr;
    if (period_ms == 0) return err_invalid_arg;
    const ticks = (@as(u64, period_ms) * loco_hz + (ms_per_sec - 1)) / ms_per_sec;
    if (ticks > std.math.maxInt(u32)) return err_invalid_arg;
    out.* = @intCast(ticks);
    return ok;
}

/// Arm ULPT0, LOCO retention, ICU/NVIC interrupt route, and WUPEN1.ULP0U.
pub fn prepare(ops: anytype, prio: u8) u16 {
    if (prio > isr_prio_max) return err_invalid_arg;

    var err = ops.ulptInit();
    if (err != ok) return err;

    err = ops.keepLoco(false);
    if (err != ok) return err;

    var slot: u16 = isr_slot_none;
    err = ops.isrLookup(elc_event_ulpt0_ulpti, &slot);
    if (err != ok) return err;
    if (slot != isr_slot_none) {
        err = ops.isrUnregister(elc_event_ulpt0_ulpti);
        if (err != ok and err != err_not_found) return err;
    }

    err = ops.isrRegister(elc_event_ulpt0_ulpti, prio);
    if (err != ok) return err;

    err = ops.armWupen1(wupen1_ulpt0u);
    if (err != ok) {
        _ = ops.isrUnregister(elc_event_ulpt0_ulpti);
        return err;
    }
    return ok;
}

/// Boundedly confirm ULPT0 started counting (ULPTCR.TCSTF = 1, HUM 25.4.7 p 1214).
pub fn waitCountStarted(ops: anytype) u16 {
    for (0..start_poll_max) |i| {
        var status: u8 = 0;
        const err = ops.ulptGetStatus(channel, &status);
        if (err != ok) return err;
        const cond = (status & tcstf_mask) != 0;
        if (ops.waitEval(@intCast(i), cond)) return ok;
    }
    return err_hw_timeout;
}

/// Execute one ULPT0-timed Software Standby sleep cycle and clean up ULPT0/PRCR.
pub fn standbyTicks(ops: anytype, period_ticks: u32) u16 {
    if (period_ticks == 0) return err_invalid_arg;

    const start_err = ops.ulptStart(channel, period_ticks);
    if (start_err != ok) return start_err;

    const wait_err = waitCountStarted(ops);
    if (wait_err != ok) {
        _ = ops.ulptStop(channel);
        return wait_err;
    }

    const unlock_err = ops.prcrUnlock();
    if (unlock_err != ok) {
        _ = ops.ulptStop(channel);
        return unlock_err;
    }

    const sleep_err = ops.enterSleep(mode_software_std);
    const relock_err = ops.prcrRelock();
    const stop_err = ops.ulptStop(channel);

    if (sleep_err != ok) return sleep_err;
    if (relock_err != ok) return relock_err;
    return stop_err;
}

/// Convert `period_ms` to ULPT0 ticks and execute one Software Standby cycle.
pub fn standbyMs(ops: anytype, period_ms: u32) u16 {
    var ticks: u32 = 0;
    const conv_err = msToTicks(period_ms, &ticks);
    if (conv_err != ok) return conv_err;
    return standbyTicks(ops, ticks);
}

/// Disarm ULPT0 wake path: stop timer, clear WUPEN1.ULP0U, release ISR slot.
pub fn disarm(ops: anytype) u16 {
    var first_err = ops.ulptStop(channel);

    const clear_err = ops.clearWupen1(wupen1_ulpt0u);
    if (clear_err != ok and first_err == ok) first_err = clear_err;

    var slot: u16 = isr_slot_none;
    const lookup_err = ops.isrLookup(elc_event_ulpt0_ulpti, &slot);
    if (lookup_err != ok) {
        if (first_err == ok) first_err = lookup_err;
        return first_err;
    }
    if (slot != isr_slot_none) {
        const unreg_err = ops.isrUnregister(elc_event_ulpt0_ulpti);
        if (unreg_err != ok and unreg_err != err_not_found and first_err == ok) {
            first_err = unreg_err;
        }
    }
    return first_err;
}
