//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host unit tests for internal/lpm_ulpt.zig (RA8FW-249).

const std = @import("std");
const lu = @import("lpm_ulpt");

const Step = enum {
    ulpt_init,
    keep_loco_off,
    isr_lookup,
    isr_unreg,
    isr_reg,
    arm_wupen1,
    ulpt_start,
    ulpt_get_status,
    wait_eval,
    prcr_unlock,
    enter_sleep,
    prcr_relock,
    ulpt_stop,
    clear_wupen1,
};

const FakeOps = struct {
    steps: [32]Step = undefined,
    n: usize = 0,
    last_step: Step = .ulpt_init,
    stop_calls: u32 = 0,
    slot_lookup: u16 = lu.isr_slot_none,
    status_val: u8 = lu.tcstf_mask,
    status_after_n: u32 = 0,
    status_reads: u32 = 0,
    fail_ulpt_init: u16 = lu.ok,
    fail_keep_loco: u16 = lu.ok,
    fail_isr_lookup: u16 = lu.ok,
    fail_isr_unreg: u16 = lu.ok,
    fail_isr_reg: u16 = lu.ok,
    fail_arm_wupen1: u16 = lu.ok,
    fail_ulpt_start: u16 = lu.ok,
    fail_ulpt_status: u16 = lu.ok,
    fail_prcr_unlock: u16 = lu.ok,
    fail_enter_sleep: u16 = lu.ok,
    fail_prcr_relock: u16 = lu.ok,
    fail_ulpt_stop: u16 = lu.ok,
    fail_clear_wupen1: u16 = lu.ok,
    last_ticks: u32 = 0,
    last_mode: u8 = 0,
    last_prio: u8 = 0,

    fn push(self: *FakeOps, s: Step) void {
        self.last_step = s;
        if (self.n < self.steps.len) {
            self.steps[self.n] = s;
            self.n += 1;
        }
    }

    pub fn ulptInit(self: *FakeOps) u16 {
        self.push(.ulpt_init);
        return self.fail_ulpt_init;
    }
    pub fn keepLoco(self: *FakeOps, stop: bool) u16 {
        if (!stop) self.push(.keep_loco_off);
        return self.fail_keep_loco;
    }
    pub fn isrLookup(self: *FakeOps, _: u16, out_slot: *u16) u16 {
        self.push(.isr_lookup);
        out_slot.* = self.slot_lookup;
        return self.fail_isr_lookup;
    }
    pub fn isrUnregister(self: *FakeOps, _: u16) u16 {
        self.push(.isr_unreg);
        self.slot_lookup = lu.isr_slot_none;
        return self.fail_isr_unreg;
    }
    pub fn isrRegister(self: *FakeOps, _: u16, prio: u8) u16 {
        self.push(.isr_reg);
        self.last_prio = prio;
        if (self.fail_isr_reg == lu.ok) self.slot_lookup = 3;
        return self.fail_isr_reg;
    }
    pub fn armWupen1(self: *FakeOps, _: u32) u16 {
        self.push(.arm_wupen1);
        return self.fail_arm_wupen1;
    }
    pub fn ulptStart(self: *FakeOps, _: u8, period: u32) u16 {
        self.push(.ulpt_start);
        self.last_ticks = period;
        return self.fail_ulpt_start;
    }
    pub fn ulptGetStatus(self: *FakeOps, _: u8, out: *u8) u16 {
        self.push(.ulpt_get_status);
        out.* = if (self.status_reads >= self.status_after_n) self.status_val else 0;
        self.status_reads += 1;
        return self.fail_ulpt_status;
    }
    pub fn waitEval(self: *FakeOps, _: u32, cond: bool) bool {
        self.push(.wait_eval);
        return cond;
    }
    pub fn prcrUnlock(self: *FakeOps) u16 {
        self.push(.prcr_unlock);
        return self.fail_prcr_unlock;
    }
    pub fn enterSleep(self: *FakeOps, mode: u8) u16 {
        self.push(.enter_sleep);
        self.last_mode = mode;
        return self.fail_enter_sleep;
    }
    pub fn prcrRelock(self: *FakeOps) u16 {
        self.push(.prcr_relock);
        return self.fail_prcr_relock;
    }
    pub fn ulptStop(self: *FakeOps, _: u8) u16 {
        self.push(.ulpt_stop);
        self.stop_calls += 1;
        return self.fail_ulpt_stop;
    }
    pub fn clearWupen1(self: *FakeOps, _: u32) u16 {
        self.push(.clear_wupen1);
        return self.fail_clear_wupen1;
    }
};

test "msToTicks validates null, zero, exact rates, and u32 overflow" {
    try std.testing.expectEqual(lu.err_null_ptr, lu.msToTicks(100, null));

    var ticks: u32 = 99;
    try std.testing.expectEqual(lu.err_invalid_arg, lu.msToTicks(0, &ticks));
    try std.testing.expectEqual(@as(u32, 99), ticks);

    try std.testing.expectEqual(lu.ok, lu.msToTicks(500, &ticks));
    try std.testing.expectEqual(@as(u32, 16384), ticks);

    try std.testing.expectEqual(lu.ok, lu.msToTicks(1, &ticks));
    try std.testing.expectEqual(@as(u32, 33), ticks);

    try std.testing.expectEqual(lu.ok, lu.msToTicks(131_071_999, &ticks));
    try std.testing.expectEqual(@as(u32, 4_294_967_264), ticks);

    try std.testing.expectEqual(lu.err_invalid_arg, lu.msToTicks(131_072_000, &ticks));
}

test "prepare arms ULPT0, keeps LOCO on, registers ISR, and sets WUPEN1" {
    var ops = FakeOps{};
    try std.testing.expectEqual(lu.ok, lu.prepare(&ops, 4));
    try std.testing.expectEqualSlices(Step, &.{
        .ulpt_init, .keep_loco_off, .isr_lookup, .isr_reg, .arm_wupen1,
    }, ops.steps[0..ops.n]);
    try std.testing.expectEqual(@as(u8, 4), ops.last_prio);
}

test "prepare unregisters prior slot cleanly and rolls back ISR on WUPEN failure" {
    var ops = FakeOps{ .slot_lookup = 2, .fail_arm_wupen1 = lu.err_invalid_arg };
    try std.testing.expectEqual(lu.err_invalid_arg, lu.prepare(&ops, 5));
    try std.testing.expectEqualSlices(Step, &.{
        .ulpt_init, .keep_loco_off, .isr_lookup, .isr_unreg, .isr_reg, .arm_wupen1, .isr_unreg,
    }, ops.steps[0..ops.n]);
    try std.testing.expectEqual(lu.err_invalid_arg, lu.prepare(&ops, 16));
}

test "standbyTicks confirms TCSTF, enters Software Standby, relocks PRCR, and stops ULPT0" {
    var ops = FakeOps{ .status_after_n = 1 };
    try std.testing.expectEqual(lu.ok, lu.standbyTicks(&ops, 16384));
    try std.testing.expectEqual(@as(u8, lu.mode_software_std), ops.last_mode);
    try std.testing.expectEqualSlices(Step, &.{
        .ulpt_start,
        .ulpt_get_status,
        .wait_eval,
        .ulpt_get_status,
        .wait_eval,
        .prcr_unlock,
        .enter_sleep,
        .prcr_relock,
        .ulpt_stop,
    }, ops.steps[0..ops.n]);
}

test "standbyTicks rejects zero and cleans up on timeout or sleep failure" {
    var ops = FakeOps{};
    try std.testing.expectEqual(lu.err_invalid_arg, lu.standbyTicks(&ops, 0));

    var timeout_ops = FakeOps{ .status_val = 0 };
    try std.testing.expectEqual(lu.err_hw_timeout, lu.standbyTicks(&timeout_ops, 100));
    try std.testing.expectEqual(@as(u32, lu.start_poll_max), timeout_ops.status_reads);
    try std.testing.expectEqual(@as(u32, 1), timeout_ops.stop_calls);
    try std.testing.expectEqual(Step.ulpt_stop, timeout_ops.last_step);

    var sleep_fail_ops = FakeOps{ .fail_enter_sleep = lu.err_invalid_arg };
    try std.testing.expectEqual(lu.err_invalid_arg, lu.standbyTicks(&sleep_fail_ops, 100));
    try std.testing.expectEqualSlices(Step, &.{
        .ulpt_start, .ulpt_get_status, .wait_eval, .prcr_unlock, .enter_sleep, .prcr_relock, .ulpt_stop,
    }, sleep_fail_ops.steps[0..sleep_fail_ops.n]);
}

test "standbyMs converts ms cleanly and disarm stops timer, clears WUPEN1, and releases ISR" {
    var ops = FakeOps{ .slot_lookup = 1 };
    try std.testing.expectEqual(lu.ok, lu.standbyMs(&ops, 250));
    try std.testing.expectEqual(@as(u32, 8192), ops.last_ticks);

    ops.n = 0;
    try std.testing.expectEqual(lu.ok, lu.disarm(&ops));
    try std.testing.expectEqualSlices(Step, &.{
        .ulpt_stop, .clear_wupen1, .isr_lookup, .isr_unreg,
    }, ops.steps[0..ops.n]);
    try std.testing.expectEqual(lu.isr_slot_none, ops.slot_lookup);
}
