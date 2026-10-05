//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GPIO pin-level ops (RA8FW-763), moved out of gpio.c: pin decode, the
//! PORTn PCNTR window, the PmnPFS address and the PWPR/PWPRS sequence.
//! HUM Ch 19 (I/O ports) and Ch 20.2 (PmnPFS, PWPR, PWPRS).

pub const port_max: u8 = 14;
pub const pin_max: u8 = 15;
pub const pin_count: usize = 16;

pub const port0_base: usize = 0x4040_0000;
pub const port_stride: usize = 0x20;
pub const off_pcntr1: usize = 0x00;
pub const off_pcntr2: usize = 0x04;
pub const off_pcntr3: usize = 0x08;
pub const high_half: u5 = 16;

pub const pfs_base: usize = 0x4040_0800;
pub const pwpr_addr: usize = 0x4040_0D0C;
pub const pwprs_addr: usize = 0x4040_0D14;
pub const pwpr_pfswe: u8 = 1 << 6;
pub const pwpr_b0wi: u8 = 1 << 7;
pub const dscr_shift: u5 = 10;
pub const dscr_mask: u32 = 0x0000_0C00;

pub const Status = enum { ok, invalid_port, invalid_pin };

pub const Pin = struct { port: u8, bit: u8 };

/// RA8_PIN_PORT / RA8_PIN_PIN with the range checks every op starts with.
pub fn decode(pin: u16) union(enum) { pin: Pin, err: Status } {
    const port: u8 = @truncate(pin >> 8);
    const bit: u8 = @truncate(pin);
    if (port > port_max) return .{ .err = .invalid_port };
    if (bit > pin_max) return .{ .err = .invalid_pin };
    return .{ .pin = .{ .port = port, .bit = bit } };
}

pub fn portReg(port: u8, off: usize) usize {
    return port0_base + @as(usize, port) * port_stride + off;
}

pub fn pfsAddr(p: Pin) usize {
    return pfs_base + (@as(usize, p.port) * pin_count + p.bit) * 4;
}

pub fn mask(p: Pin) u32 {
    return @as(u32, 1) << @intCast(p.bit);
}

/// PCNTR3 value: POSR (low half) sets the pin, PORR (high half) clears it.
pub fn writeValue(p: Pin, high: bool) u32 {
    return if (high) mask(p) else mask(p) << high_half;
}

/// Toggle from the current PCNTR1 (PODR is bits 31:16).
pub fn toggleValue(p: Pin, pcntr1: u32) u32 {
    return writeValue(p, ((pcntr1 >> high_half) & mask(p)) == 0);
}

pub fn levelOf(p: Pin, pcntr2: u32) bool {
    return (pcntr2 & mask(p)) != 0;
}

pub fn withDscr(pfs: u32, dscr: u8) u32 {
    return (pfs & ~dscr_mask) | ((@as(u32, dscr) << dscr_shift) & dscr_mask);
}

/// HUM 20.2.5/20.2.6: B0WI cleared before PFSWE, both NS and Secure paths.
pub fn unlock(hw: anytype) void {
    hw.write8(pwpr_addr, 0);
    hw.write8(pwpr_addr, pwpr_pfswe);
    hw.write8(pwprs_addr, 0);
    hw.write8(pwprs_addr, pwpr_pfswe);
}

pub fn lock(hw: anytype) void {
    hw.write8(pwpr_addr, 0);
    hw.write8(pwpr_addr, pwpr_b0wi);
    hw.write8(pwprs_addr, 0);
    hw.write8(pwprs_addr, pwpr_b0wi);
}

// ---- Init, routing and IRQ helpers (RA8FW-767) ------------------------

pub const pfs_podr: u32 = 0x0000_0001;
pub const pfs_pdr: u32 = 0x0000_0004;
pub const pfs_pcr: u32 = 0x0000_0010;
pub const pfs_pmr: u32 = 0x0001_0000;
pub const psel_shift: u5 = 24;
pub const pull_up: u8 = 1;
pub const irq_num_max: u8 = 15;
pub const irq_event_base: u16 = 1;

/// Output: PDR, plus PODR when the initial level is high.
pub fn outputValue(high: bool) u32 {
    return pfs_pdr | (if (high) pfs_podr else 0);
}

/// Input: PDR clear; PCR only for pull-up (pull-down has no PFS bit).
pub fn inputValue(pull: u8) u32 {
    return if (pull == pull_up) pfs_pcr else 0;
}

/// HUM 20.2.4: clear PMR, write PSEL with PMR = 0, then set PMR.
pub fn routeSteps(psel: u8) [3]u32 {
    const sel = @as(u32, psel) << psel_shift;
    return .{ 0, sel, pfs_pmr | sel };
}

/// ELC event for external IRQn (IRQ0 is event 1).
pub fn irqEvent(irq_num: u8) u16 {
    return irq_event_base + irq_num;
}

/// Unlock PWPR/PWPRS, write each value to the PmnPFS register, re-lock.
pub fn program(hw: anytype, addr: usize, values: []const u32) void {
    unlock(hw);
    for (values) |v| hw.write32(addr, v);
    lock(hw);
}
