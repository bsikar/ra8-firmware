//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RIIC controller bus primitives (RA8FW-886, was part of ra8_i2c.c):
//! ICSR2 flag waits, status mapping and clear, START / repeated START,
//! STOP, NACK, the BBSY busy gate and the address byte. Used by
//! i2c_xfer.zig. HUM Ch 39.

pub const off_iccr2: usize = 0x01;
pub const off_icmr3: usize = 0x04;
pub const off_icsr2: usize = 0x09;
pub const off_icdrt: usize = 0x12;

pub const iccr2_st: u8 = 1 << 1;
pub const iccr2_rs: u8 = 1 << 2;
pub const iccr2_sp: u8 = 1 << 3;
pub const iccr2_bbsy: u8 = 1 << 7;
pub const icmr3_ackbt: u8 = 1 << 3;
pub const icmr3_ackwp: u8 = 1 << 4;
pub const icsr2_al: u8 = 1 << 1;
pub const icsr2_start: u8 = 1 << 2;
pub const icsr2_stop: u8 = 1 << 3;
pub const icsr2_nackf: u8 = 1 << 4;
pub const icsr2_tdre: u8 = 1 << 7;
/// START, STOP, NACKF and AL are W0C; clear() writes them back as 0.
pub const clear_mask: u8 = icsr2_start | icsr2_stop | icsr2_nackf | icsr2_al;

/// `k_ra8_i2c_poll_limit`: spins per flag wait.
pub const poll_limit: u32 = 200000;

pub const busy: u16 = 0x109;
pub const hw_timeout: u16 = 0x203;
pub const hw_error: u16 = 0x204;
pub const nack: u16 = 0x407;

fn setBits(regs: anytype, off: usize, bits: u8) void {
    regs.write8(off, regs.read8(off) | bits);
}

fn clearBits(regs: anytype, off: usize, bits: u8) void {
    regs.write8(off, regs.read8(off) & ~bits);
}

/// Spin until any bit of `mask` is set in ICSR2 (HUM 39.2.10, p 2384).
pub fn waitIcsr2(regs: anytype, mask: u8) u16 {
    var i: u32 = 0;
    while (i < poll_limit) : (i += 1) {
        if (regs.poll(off_icsr2, i, regs.read8(off_icsr2) & mask != 0)) return 0;
    }
    return hw_timeout;
}

/// NACKF wins over AL; neither set is success.
pub fn status(icsr2: u8) u16 {
    if (icsr2 & icsr2_nackf != 0) return nack;
    if (icsr2 & icsr2_al != 0) return hw_error;
    return 0;
}

pub fn clearStatus(regs: anytype) void {
    clearBits(regs, off_icsr2, clear_mask);
}

/// START on an idle bus, else repeated START. ICDRT drops writes while
/// RS = 1 (HUM 39.11, p 2434), so wait for RS to self-clear.
pub fn open(regs: anytype, bus_held: bool) void {
    if (!bus_held) return setBits(regs, off_iccr2, iccr2_st);
    setBits(regs, off_iccr2, iccr2_rs);
    var i: u32 = 0;
    while (i < poll_limit) : (i += 1) {
        if (regs.poll(off_iccr2, i, regs.read8(off_iccr2) & iccr2_rs == 0)) return;
    }
}

pub fn waitFree(regs: anytype) void {
    var i: u32 = 0;
    while (i < poll_limit) : (i += 1) {
        if (regs.read8(off_iccr2) & iccr2_bbsy == 0) return;
    }
}

/// Clear the latched STOP flag, then request a stop condition.
pub fn stopRequest(regs: anytype) void {
    clearBits(regs, off_icsr2, icsr2_stop);
    setBits(regs, off_iccr2, iccr2_sp);
}

pub fn stop(regs: anytype) void {
    stopRequest(regs);
    waitFree(regs);
}

/// ACKBT is write-protected by ACKWP (HUM 39.2.5, p 2376).
pub fn setNack(regs: anytype) void {
    setBits(regs, off_icmr3, icmr3_ackwp);
    setBits(regs, off_icmr3, icmr3_ackbt);
    clearBits(regs, off_icmr3, icmr3_ackwp);
}

pub fn busyGate(regs: anytype, bus_held: bool) u16 {
    if (bus_held) return 0;
    return if (regs.read8(off_iccr2) & iccr2_bbsy == 0) 0 else busy;
}

/// Wait for TDRE, write the address byte, report NACKF / AL.
pub fn sendAddress(regs: anytype, byte: u8) u16 {
    const rc = waitIcsr2(regs, icsr2_tdre);
    if (rc != 0) return rc;
    regs.write8(off_icdrt, byte);
    return status(regs.read8(off_icsr2));
}
