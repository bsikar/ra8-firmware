//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! gPTP timer driver (RA8FW-589, was ra8_eth_gptp.c). Pure: the GPTP block
//! comes in through a `regs` value (32-bit read/write by offset) and MSTP
//! and logging through an `ops` value. HUM Ch 35 p 1926..1947.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const not_initialized: u16 = 0x10F;
pub const out_of_range: u16 = 0x208;
pub const null_ptr: u16 = 0x504;

pub const base_addr: usize = 0x403E_0000;
pub const off_ipv: usize = 0x00;
pub const off_tmec: usize = 0x10;
pub const off_tmdc: usize = 0x14;
pub const timer_base: usize = 0x20;
pub const timer_stride: usize = 0x40;
pub const t_tivc: usize = 0x00;
pub const t_tovcl: usize = 0x10;
pub const t_tovcm: usize = 0x14;
pub const t_tovcu: usize = 0x18;
pub const t_avtpl: usize = 0x20;
pub const t_avtpu: usize = 0x24;
pub const t_gptpl: usize = 0x30;
pub const t_gptpm: usize = 0x34;
pub const t_gptpu: usize = 0x38;

pub const timer_count: u8 = 2;
/// MSTPC30: the Layer-3 Ethernet Switch domain that holds GPTP.
pub const mstp_eswm: u16 = (2 << 8) | 30;
pub const ns_per_sec: u64 = 1_000_000_000;
pub const nsec_max: u32 = 999_999_999;
pub const sec_max: u64 = 0xFFFF_FFFF_FFFF;
const subns_shift = 27; // PTPTIVCt.TIV is 5.27 fixed point
const mask_nsec: u32 = 0x3FFF_FFFF;
const mask_sec_u: u32 = 0xFFFF;

const not_init_msg = "ra8_eth_gptp_init has not run";

pub fn timerOff(timer: u8, reg: usize) usize {
    return timer_base + timer_stride * @as(usize, timer) + reg;
}

/// TIV = round(1e9 * 2^27 / clk_hz) (HUM 35.3.2.3 p 1928).
pub fn tivFromHz(clk_hz: u32) error{ InvalidArg, OutOfRange }!u32 {
    if (clk_hz == 0) return error.InvalidArg;
    const tiv = ((ns_per_sec << subns_shift) + clk_hz / 2) / clk_hz;
    if (tiv > 0xFFFF_FFFF) return error.OutOfRange;
    return @intCast(tiv);
}

pub fn tivCode(r: error{ InvalidArg, OutOfRange }!u32) u16 {
    _ = r catch |e| return if (e == error.InvalidArg) invalid_arg else out_of_range;
    return ok;
}

fn stop(regs: anytype, timer: u8) void {
    regs.write32(off_tmdc, @as(u32, 1) << @intCast(timer));
}

/// U then M then L: the L write commits the 78-bit offset (HUM p 1944).
fn writeOffset(regs: anytype, timer: u8, sec: u64, nsec: u32) void {
    regs.write32(timerOff(timer, t_tovcu), @as(u32, @truncate(sec >> 32)) & mask_sec_u);
    regs.write32(timerOff(timer, t_tovcm), @truncate(sec));
    regs.write32(timerOff(timer, t_tovcl), nsec & mask_nsec);
}

fn resetTimers(regs: anytype, tiv: u32) void {
    var t: u8 = 0;
    while (t < timer_count) : (t += 1) {
        stop(regs, t);
        regs.write32(timerOff(t, t_tivc), tiv);
        writeOffset(regs, t, 0, 0);
    }
}

pub const Time = struct { sec: u64, nsec: u32 };

/// The configured flag guards every register access: with MSTPC30 still set
/// a GPTP access bus-faults. It survives a module stop; only deinit clears it.
pub const State = struct {
    configured: bool = false,

    fn guard(s: *const State, ops: anytype) ?u16 {
        if (s.configured) return null;
        ops.logError(not_init_msg);
        return not_initialized;
    }

    fn guardTimer(s: *const State, ops: anytype, timer: u8) ?u16 {
        if (s.guard(ops)) |e| return e;
        if (timer >= timer_count) return invalid_arg;
        return null;
    }

    pub fn init(s: *State, regs: anytype, ops: anytype, clk_hz: u32) u16 {
        const tiv = tivFromHz(clk_hz) catch |e| {
            const code = tivCode(e);
            ops.fail("gptp_init: clk_hz", code);
            return code;
        };
        const m = ops.mstpEnable(mstp_eswm);
        if (m != ok) {
            ops.fail("gptp_init: mstp enable", m);
            return m;
        }
        resetTimers(regs, tiv);
        s.configured = true;
        ops.logInfo("gptp_init");
        return ok;
    }

    pub fn deinit(s: *State, regs: anytype, ops: anytype) u16 {
        if (s.guard(ops)) |e| return e;
        resetTimers(regs, 0);
        s.configured = false;
        return ops.mstpDisable(mstp_eswm);
    }

    pub fn ipVersion(s: *const State, regs: anytype, ops: anytype, out: *u32) u16 {
        if (s.guard(ops)) |e| return e;
        out.* = regs.read32(off_ipv);
        return ok;
    }

    pub fn enable(s: *const State, regs: anytype, ops: anytype, timer: u8) u16 {
        if (s.guardTimer(ops, timer)) |e| return e;
        regs.write32(off_tmec, @as(u32, 1) << @intCast(timer));
        return ok;
    }

    pub fn disable(s: *const State, regs: anytype, ops: anytype, timer: u8) u16 {
        if (s.guardTimer(ops, timer)) |e| return e;
        stop(regs, timer);
        return ok;
    }

    pub fn isEnabled(s: *const State, regs: anytype, ops: anytype, timer: u8, out: *bool) u16 {
        if (s.guardTimer(ops, timer)) |e| return e;
        out.* = (regs.read32(off_tmec) >> @intCast(timer)) & 1 != 0;
        return ok;
    }

    pub fn setIncrement(s: *const State, regs: anytype, ops: anytype, timer: u8, tiv: u32) u16 {
        if (s.guardTimer(ops, timer)) |e| return e;
        if (tiv == 0) return invalid_arg;
        regs.write32(timerOff(timer, t_tivc), tiv);
        return ok;
    }

    pub fn getIncrement(s: *const State, regs: anytype, ops: anytype, timer: u8, out: *u32) u16 {
        if (s.guardTimer(ops, timer)) |e| return e;
        out.* = regs.read32(timerOff(timer, t_tivc));
        return ok;
    }

    pub fn setOffset(s: *const State, regs: anytype, ops: anytype, timer: u8, sec: u64, nsec: u32) u16 {
        if (s.guardTimer(ops, timer)) |e| return e;
        if (sec > sec_max or nsec > nsec_max) return invalid_arg;
        writeOffset(regs, timer, sec, nsec);
        return ok;
    }

    /// Reads L first: that read latches M and U (HUM Figure 35.7 p 1946).
    pub fn time(s: *const State, regs: anytype, ops: anytype, timer: u8, out: *Time) u16 {
        if (s.guardTimer(ops, timer)) |e| return e;
        const nsec = regs.read32(timerOff(timer, t_gptpl)) & mask_nsec;
        const lo: u64 = regs.read32(timerOff(timer, t_gptpm));
        const hi: u64 = regs.read32(timerOff(timer, t_gptpu)) & mask_sec_u;
        out.* = .{ .sec = (hi << 32) | lo, .nsec = nsec };
        return ok;
    }

    /// Reads L first: that read latches U (HUM Figure 35.6 p 1945).
    pub fn avtpNs(s: *const State, regs: anytype, ops: anytype, timer: u8, out: *u64) u16 {
        if (s.guardTimer(ops, timer)) |e| return e;
        const lo: u64 = regs.read32(timerOff(timer, t_avtpl));
        const hi: u64 = regs.read32(timerOff(timer, t_avtpu));
        out.* = (hi << 32) | lo;
        return ok;
    }

    pub fn enterStop(s: *const State, regs: anytype, ops: anytype) u16 {
        if (s.guard(ops)) |e| return e;
        var t: u8 = 0;
        while (t < timer_count) : (t += 1) stop(regs, t);
        return ops.mstpDisable(mstp_eswm);
    }

    pub fn exitStop(s: *const State, ops: anytype) u16 {
        if (s.guard(ops)) |e| return e;
        return ops.mstpEnable(mstp_eswm);
    }
};
