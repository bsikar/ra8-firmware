//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_lpm.h (RA8FW-792), replacing ra8_lpm.c. The logic is in
//! internal/lpm.zig; this file keeps the null checks, the log strings and the
//! volatile access at SYSC 0x4001E000, ICU 0x4000C000 and SCB SCR 0xE000ED10.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const lpm = @import("internal/lpm.zig");

const tag = "LPM";
const ok = lpm.codes.ok;
const freestanding = builtin.os.tag == .freestanding;

const Hw = struct {
    fn reg(comptime T: type, a: usize) *volatile T {
        return @ptrFromInt(a);
    }
    pub fn read8(_: Hw, a: usize) u8 {
        return reg(u8, a).*;
    }
    pub fn write8(_: Hw, a: usize, v: u8) void {
        reg(u8, a).* = v;
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

extern fn ra8_hw_dsb() void;
extern fn ra8_hw_wfi() void;

fn waitForInterrupt() void {
    if (freestanding) {
        asm volatile ("dsb 0xF" ::: .{ .memory = true });
        asm volatile ("wfi" ::: .{ .memory = true });
    } else {
        ra8_hw_dsb();
        ra8_hw_wfi();
    }
}

fn fail(msg: [*:0]const u8, code: u16) u16 {
    common.ra8_log_emit_error(tag, msg);
    return code;
}

fn nullOut(msg: [*:0]const u8) u16 {
    return fail(msg, lpm.codes.null_ptr);
}

export fn ra8_lpm_init(cfg: ?*const lpm.Config) u16 {
    const c = cfg orelse return nullOut("cfg must not be nullptr");
    lpm.init(hw, c.*);
    common.ra8_log_emit_info(tag, "lpm_init");
    return ok;
}

export fn ra8_lpm_deinit() u16 {
    lpm.deinit(hw);
    common.ra8_log_emit_info(tag, "lpm_deinit");
    return ok;
}

export fn ra8_lpm_prcr_unlock() u16 {
    lpm.setPrc1(hw, true);
    return ok;
}

export fn ra8_lpm_prcr_relock() u16 {
    lpm.setPrc1(hw, false);
    return ok;
}

export fn ra8_lpm_set_wakeup_sources(wupen0: u32, wupen1: u32) u16 {
    hw.write32(lpm.icu + lpm.off.wupen0, wupen0);
    hw.write32(lpm.icu + lpm.off.wupen1, wupen1);
    return ok;
}

export fn ra8_lpm_arm_wupen0_bits(bits: u32) u16 {
    lpm.setWupen(hw, lpm.off.wupen0, bits, true);
    return ok;
}

export fn ra8_lpm_clear_wupen0_bits(bits: u32) u16 {
    lpm.setWupen(hw, lpm.off.wupen0, bits, false);
    return ok;
}

export fn ra8_lpm_arm_wupen1_bits(bits: u32) u16 {
    lpm.setWupen(hw, lpm.off.wupen1, bits, true);
    return ok;
}

export fn ra8_lpm_clear_wupen1_bits(bits: u32) u16 {
    lpm.setWupen(hw, lpm.off.wupen1, bits, false);
    return ok;
}

export fn ra8_lpm_arm_dpsier(idx: u8, value: u8) u16 {
    if (idx >= lpm.dpsi_count) return fail("arm_dpsier: idx out of range", lpm.codes.invalid_arg);
    lpm.armDpsier(hw, idx, value);
    return ok;
}

export fn ra8_lpm_clear_dpsifr(idx: u8) u16 {
    if (idx >= lpm.dpsi_count) return fail("clear_dpsifr: idx out of range", lpm.codes.invalid_arg);
    lpm.clearFlags(hw, idx);
    return ok;
}

export fn ra8_lpm_set_dpsiegr(idx: u8, value: u8) u16 {
    if (idx >= lpm.dpsi_count) return fail("set_dpsiegr: idx out of range", lpm.codes.invalid_arg);
    hw.write8(lpm.sysc + lpm.edgeOff(idx), value);
    return ok;
}

export fn ra8_lpm_snooze_set_request_sources(ulpt0_underflow: bool, ulpt1_underflow: bool, acmphs0: bool) u16 {
    lpm.snoozeRequest(hw, ulpt0_underflow, ulpt1_underflow, acmphs0);
    return ok;
}

export fn ra8_lpm_snooze_set_end_sources(ulpt0: bool, ulpt1: bool, usbfs: bool, usbhs: bool) u16 {
    lpm.snoozeEnd(hw, ulpt0, ulpt1, usbfs, usbhs);
    return ok;
}

export fn ra8_lpm_set_ram_retention(cfg: ?*const lpm.RamRetention) u16 {
    const c = cfg orelse return nullOut("ram retention cfg null");
    lpm.ramRetention(hw, c.*);
    return ok;
}

export fn ra8_lpm_set_ldo_standby(cfg: ?*const lpm.LdoCfg) u16 {
    const c = cfg orelse return nullOut("ldo cfg null");
    const rc = lpm.ldoStandby(hw, c.*);
    if (rc != ok) return fail("set_ldo: OPCM!=0", rc);
    return ok;
}

export fn ra8_lpm_set_clock_stop(clock: u8, stop: bool) u16 {
    if (clock >= lpm.clock_count) return fail("set_clock_stop: bad clock", lpm.codes.invalid_arg);
    lpm.clockStop(hw, clock, stop);
    return ok;
}

export fn ra8_lpm_get_clock_stop(clock: u8, stop: ?*bool) u16 {
    const out = stop orelse return nullOut("stop must not be nullptr");
    if (clock >= lpm.clock_count) return fail("get_clock_stop: bad clock", lpm.codes.invalid_arg);
    out.* = lpm.clockStopped(hw, clock);
    return ok;
}

export fn ra8_lpm_get_opccr(opccr: ?*u8) u16 {
    const out = opccr orelse return nullOut("opccr must not be nullptr");
    out.* = hw.read8(lpm.sysc + lpm.off.opccr);
    return ok;
}

export fn ra8_lpm_wait_for_opccr(poll_limit: u32) u16 {
    if (poll_limit == 0) return lpm.codes.invalid_arg;
    if (lpm.waitOpccr(hw, poll_limit)) return ok;
    return fail("wait_for_opccr timeout", lpm.codes.hw_timeout);
}

export fn ra8_lpm_enter_sleep(mode: u8) u16 {
    if (!lpm.validMode(mode)) {
        common.ra8_log_emit_error(tag, "lpm_enter_sleep: bad mode");
        common.ra8_log_emit_error_val(tag, "Error", lpm.codes.invalid_arg);
        return lpm.codes.invalid_arg;
    }
    lpm.armSleep(hw, mode);
    common.ra8_log_emit_info_val(tag, "lpm_enter_sleep mode", mode);
    waitForInterrupt();
    lpm.disarmSleep(hw);
    return ok;
}

export fn ra8_lpm_enter_deep_standby() u16 {
    return ra8_lpm_enter_sleep(lpm.deep_standby_1);
}

export fn ra8_lpm_get_status(out: ?*u32) u16 {
    const p = out orelse return nullOut("out must not be nullptr");
    p.* = lpm.status(hw);
    return ok;
}

export fn ra8_lpm_get_exit_cause(out: ?*u64) u16 {
    const p = out orelse return nullOut("out must not be nullptr");
    p.* = lpm.cause(hw);
    return ok;
}

export fn ra8_lpm_get_dpsi_state(enables: ?*[4]u8, flags: ?*[4]u8, edges: ?*[3]u8) u16 {
    const en = enables orelse return nullOut("enables nullptr");
    const fl = flags orelse return nullOut("flags nullptr");
    const eg = edges orelse return nullOut("edges nullptr");
    lpm.dpsiState(hw, en, fl, eg);
    return ok;
}
