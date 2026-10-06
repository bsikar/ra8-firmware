//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RIIC controller transfers (RA8FW-887, the rest of ra8_i2c.c): polled
//! write with an optional held bus, polled read with the WAIT / NACK /
//! STOP end-of-frame sequence, and the address probe. Built on
//! i2c_bus.zig; exports live in src/i2c_xfer_abi.zig. HUM Ch 39.3.

const bus = @import("i2c_bus.zig");

pub const off_icdrr: usize = 0x13;
pub const icmr3_wait: u8 = 1 << 6;
pub const icsr2_rdrf: u8 = 1 << 5;
pub const icsr2_tend: u8 = 1 << 6;

/// 7-bit address on the wire: shifted left, R/W in bit 0.
pub fn addressByte(addr7: u8, is_read: bool) u8 {
    return (addr7 << 1) | @intFromBool(is_read);
}

fn setBits(regs: anytype, off: usize, bits: u8) void {
    regs.write8(off, regs.read8(off) | bits);
}

/// Busy gate, clear status, START or repeated START, address byte. On an
/// address failure the bus is stopped and released.
fn begin(regs: anytype, held: *bool, byte: u8) u16 {
    const gate = bus.busyGate(regs, held.*);
    if (gate != 0) return gate;
    bus.clearStatus(regs);
    bus.open(regs, held.*);
    const rc = bus.sendAddress(regs, byte);
    if (rc != 0) {
        bus.stop(regs);
        held.* = false;
    }
    return rc;
}

/// One byte per TDRE; NACKF aborts before the next write (HUM 39.2.10).
pub fn drainTx(regs: anytype, data: []const u8) u16 {
    for (data) |b| {
        const rc = bus.waitIcsr2(regs, bus.icsr2_tdre);
        if (rc != 0) return rc;
        if (regs.read8(bus.off_icsr2) & bus.icsr2_nackf != 0) return bus.nack;
        regs.write8(bus.off_icdrt, b);
    }
    return 0;
}

/// Wait for TEND, then STOP on error or `send_stop`, else hold the bus
/// for a repeated START (Controller Transmit step 5, HUM 39.3.3).
pub fn finishTx(regs: anytype, err: u16, send_stop: bool, held: *bool) u16 {
    var rc = err;
    if (rc == 0) rc = bus.waitIcsr2(regs, icsr2_tend);
    if (rc != 0 or send_stop) {
        bus.stop(regs);
        bus.clearStatus(regs);
        held.* = false;
    } else {
        held.* = true;
    }
    return rc;
}

pub fn write(regs: anytype, held: *bool, addr7: u8, data: []const u8, send_stop: bool) u16 {
    const rc = begin(regs, held, addressByte(addr7, false));
    if (rc != 0) return rc;
    return finishTx(regs, drainTx(regs, data), send_stop, held);
}

/// End-of-frame control for `remain` bytes still to read (HUM 39.3.4).
fn armTail(regs: anytype, remain: usize) void {
    switch (remain) {
        3 => setBits(regs, bus.off_icmr3, icmr3_wait),
        2 => bus.setNack(regs),
        1 => bus.stopRequest(regs),
        else => {},
    }
}

/// First RDRF ends the address phase; short reads arm WAIT / NACK before
/// the dummy ICDRR read starts the data clock (HUM 39.3.4, p 2400).
pub fn drainRx(regs: anytype, out: []u8) u16 {
    var rc = bus.waitIcsr2(regs, icsr2_rdrf);
    if (rc != 0) return rc;
    if (out.len <= 2) setBits(regs, bus.off_icmr3, icmr3_wait);
    if (out.len == 1) bus.setNack(regs);
    _ = regs.read8(off_icdrr);
    for (out, 0..) |*slot, loaded| {
        rc = bus.waitIcsr2(regs, icsr2_rdrf);
        if (rc != 0) break;
        armTail(regs, out.len - loaded);
        slot.* = regs.read8(off_icdrr);
    }
    // Clearing WAIT lets the requested STOP fire; ACKBT resets for the
    // next transfer (HUM 39.2.5, p 2376).
    const keep = ~(icmr3_wait | bus.icmr3_ackbt);
    regs.write8(bus.off_icmr3, regs.read8(bus.off_icmr3) & keep);
    bus.waitFree(regs);
    return rc;
}

pub fn read(regs: anytype, held: *bool, addr7: u8, out: []u8) u16 {
    var rc = begin(regs, held, addressByte(addr7, true));
    if (rc != 0) return rc;
    rc = drainRx(regs, out);
    const st = bus.status(regs.read8(bus.off_icsr2));
    if (st != 0) rc = st;
    bus.clearStatus(regs);
    held.* = false;
    return rc;
}

/// Address-only probe. An address NACK is a valid outcome: `acked` is
/// false and the call succeeds once TEND or NACKF lands.
pub fn scan(regs: anytype, held: *bool, addr7: u8, acked: *bool) u16 {
    acked.* = false;
    const gate = bus.busyGate(regs, held.*);
    if (gate != 0) return gate;
    bus.clearStatus(regs);
    bus.open(regs, false);
    var rc = bus.sendAddress(regs, addressByte(addr7, false));
    if (rc != 0 and rc != bus.nack) {
        bus.stop(regs);
        held.* = false;
        return rc;
    }
    rc = bus.waitIcsr2(regs, icsr2_tend | bus.icsr2_nackf);
    if (rc == 0) acked.* = regs.read8(bus.off_icsr2) & bus.icsr2_nackf == 0;
    bus.stop(regs);
    bus.clearStatus(regs);
    held.* = false;
    return rc;
}
