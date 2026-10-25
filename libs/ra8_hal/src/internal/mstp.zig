//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Ref-counted MSTP module-stop gating logic (RA8FW-770). The five MSTPCR
//! registers clock-gate every peripheral; the ref-counted gate lets two
//! unrelated drivers share one MSTP bit (DMAC + DTC, OSPI + DOTF, SSIE0/1)
//! without trampling each other. The logic is generic over a `Hw` value
//! (register access, PSAR reads, the bounded poll, logging) so host tests
//! drive a fake; the ABI keeps one private gate, just as the C kept
//! `s_refcount` file-static.

const std = @import("std");
const common = @import("../abi_common.zig");

pub const k_ra8_ok = common.k_ra8_ok;
pub const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
pub const k_ra8_err_invalid_state = common.k_ra8_err_invalid_state;
pub const k_ra8_err_hw_timeout = common.k_ra8_err_hw_timeout;
pub const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;

/// MSTPCRA..MSTPCRE.
pub const reg_count = 5;

/// 32 bits per register.
pub const bit_count = 32;

/// Bounded spin budget for the read-back protocol (HUM 11.2.6 N2). The MSTP
/// bit updates on the next bus cycle, so the realistic worst case is single
/// digits; 0x100 keeps the loop unconditionally bounded (NASA Power of 10
/// Rule 2).
pub const readback_spin = 0x100;

/// All-stopped pattern for MSTPCRB..E. Reserved bits read-as-1/write-1.
pub const all_stopped: u32 = 0xFFFF_FFFF;

/// MSTPCRA keeps bits 0..3 (SRAM0..3) at 0 (running): stopping the ECC SRAM
/// controllers that back the CPU stack bus-faults the next stack access
/// (HUM Ch 11.2.6 p 443, MSTPA0..MSTPA3).
pub const safe_stopped_a: u32 = 0xFFFF_FFF0;

/// PSARB..PSARE addresses (R_PSCU +0x04..+0x10). MSTPCRA has no PSAR word,
/// so its whole width stays Secure-owned. R_PSCU is Secure-only, so the
/// reads stay at these addresses on every world.
pub const psar_addrs = [reg_count]usize{
    0,           0x4020_4004,
    0x4020_4008, 0x4020_400C,
    0x4020_4010,
};

const zero_row: [bit_count]u8 = @splat(0);
const zero_gate: [reg_count][bit_count]u8 = @splat(zero_row);

/// Per-(register, bit) usage counters. 0 means no driver currently needs the
/// peripheral and the MSTPCR bit should read 1 (stopped).
pub const Gate = struct {
    counts: [reg_count][bit_count]u8 = zero_gate,
};

/// A decoded packed id: which register and which bit.
pub const Decoded = struct { reg: u8, bit: u8 };

/// Decode a packed `(reg << 8) | bit` id; out-of-range fields return null.
pub fn decode(id: u16) ?Decoded {
    const reg: u8 = @truncate(id >> 8);
    const bit: u8 = @truncate(id);
    if (reg >= reg_count or bit >= bit_count) return null;
    return .{ .reg = reg, .bit = bit };
}

/// Non-secure-owned bit mask for one MSTPCR register: the PSAR value for
/// B..E, 0 for A (no attribution register) and on any non-TrustZone system
/// (every PSAR reads 0). A module-stop bit follows its peripheral's PSAR
/// attribution: once the Secure side marks a peripheral Non-secure, a
/// Secure write to its bit is masked and the bit never takes the commanded
/// state (HUM Ch 51.8.1 p 3284).
pub fn nsMask(hw: anytype, reg: u8) u32 {
    return hw.psar(reg);
}

/// Bounded masked settle poll (the init read-back): succeeds when the
/// cared-for bits read back `expect`. Bits outside `care_mask` are
/// Non-secure-owned and ignore Secure writes by design, so they never take
/// the commanded state and must not hold the poll open.
pub fn waitRegSettle(hw: anytype, reg: u8, expect: u32, care_mask: u32) u16 {
    var iter: u16 = 0;
    while (iter < readback_spin) : (iter += 1) {
        const settled = (hw.read(reg) & care_mask) == (expect & care_mask);
        if (hw.poll(reg, iter, settled)) return k_ra8_ok;
    }
    return k_ra8_err_hw_timeout;
}

/// Bounded single-bit read-back (HUM 11.2.6 Note 2): poll until the bit
/// reads back at the commanded gate state.
fn waitBit(hw: anytype, reg: u8, bit: u8, expected_stopped: bool) u16 {
    var iter: u16 = 0;
    while (iter < readback_spin) : (iter += 1) {
        const settled = (((hw.read(reg) >> @as(u5, @intCast(bit))) & 1) != 0) == expected_stopped;
        if (hw.poll(reg, iter, settled)) return k_ra8_ok;
    }
    return k_ra8_err_hw_timeout;
}

/// `ra8_mstp_init`: clear every refcount, stop every module (MSTPCRA keeps
/// SRAM0..3 running) in order A..E, then read each register back against
/// the bits the Secure side actually owns.
pub fn init(hw: anytype, gate: *Gate) u16 {
    hw.info("ra8_mstp_init -- gating all modules");
    gate.* = .{};
    const init_vals = [reg_count]u32{
        safe_stopped_a, all_stopped, all_stopped, all_stopped, all_stopped,
    };
    for (0..reg_count) |r| {
        hw.write(@intCast(r), init_vals[r]);
    }
    for (0..reg_count) |r| {
        const reg: u8 = @intCast(r);
        const care = ~nsMask(hw, reg);
        if (waitRegSettle(hw, reg, init_vals[r], care) != k_ra8_ok) {
            hw.errVal("init read-back failed reg", @as(u32, reg));
            return k_ra8_err_hw_timeout;
        }
    }
    return k_ra8_ok;
}

/// `ra8_mstp_enable`: take one reference; on the 0->1 edge clear the MSTPCR
/// bit (ungate) and read it back, rolling the count back on a timeout so a
/// caller retry starts fresh.
pub fn enable(hw: anytype, gate: *Gate, id: u16) u16 {
    const d = decode(id) orelse {
        hw.errVal("enable: invalid id", @as(u32, id));
        return k_ra8_err_invalid_arg;
    };
    const prev = gate.counts[d.reg][d.bit];
    if (prev == std.math.maxInt(u8)) {
        // Defensive: 255 simultaneous users would mean a bug elsewhere.
        hw.errVal("enable: refcount saturated", @as(u32, id));
        return k_ra8_err_invalid_state;
    }
    gate.counts[d.reg][d.bit] = prev + 1;
    if (prev != 0) {
        // Already running. Nothing else to do.
        return k_ra8_ok;
    }
    const mask = @as(u32, 1) << @as(u5, @intCast(d.bit));
    hw.write(d.reg, hw.read(d.reg) & ~mask);
    if (waitBit(hw, d.reg, d.bit, false) != k_ra8_ok) {
        gate.counts[d.reg][d.bit] = prev;
        hw.errVal("enable: read-back timeout id", @as(u32, id));
        return k_ra8_err_hw_timeout;
    }
    return k_ra8_ok;
}

/// `ra8_mstp_disable`: drop one reference; on the 1->0 edge set the MSTPCR
/// bit (gate) and read it back, rolling the count back on a timeout so the
/// next caller still sees the resource as in use.
pub fn disable(hw: anytype, gate: *Gate, id: u16) u16 {
    const d = decode(id) orelse {
        hw.errVal("disable: invalid id", @as(u32, id));
        return k_ra8_err_invalid_arg;
    };
    const prev = gate.counts[d.reg][d.bit];
    if (prev == 0) {
        hw.errVal("disable: refcount underflow", @as(u32, id));
        return k_ra8_err_invalid_state;
    }
    gate.counts[d.reg][d.bit] = prev - 1;
    if (prev != 1) {
        // Other users still active. Leave the bit cleared.
        return k_ra8_ok;
    }
    const mask = @as(u32, 1) << @as(u5, @intCast(d.bit));
    hw.write(d.reg, hw.read(d.reg) | mask);
    if (waitBit(hw, d.reg, d.bit, true) != k_ra8_ok) {
        gate.counts[d.reg][d.bit] = prev;
        hw.errVal("disable: read-back timeout id", @as(u32, id));
        return k_ra8_err_hw_timeout;
    }
    return k_ra8_ok;
}

/// `ra8_mstp_get_refcount`.
pub fn refcount(hw: anytype, gate: *const Gate, id: u16, out_ref: ?*u8) u16 {
    if (out_ref == null) {
        hw.errVal("get_refcount: out_ref", 0);
        return k_ra8_err_null_ptr;
    }
    const d = decode(id) orelse return k_ra8_err_invalid_arg;
    out_ref.?.* = gate.counts[d.reg][d.bit];
    return k_ra8_ok;
}

/// `ra8_mstp_is_stopped`.
pub fn stopped(hw: anytype, gate: *const Gate, id: u16, out_stopped: ?*bool) u16 {
    _ = gate;
    if (out_stopped == null) {
        hw.errVal("is_stopped: out_stopped", 0);
        return k_ra8_err_null_ptr;
    }
    const d = decode(id) orelse return k_ra8_err_invalid_arg;
    out_stopped.?.* = ((hw.read(d.reg) >> @as(u5, @intCast(d.bit))) & 1) != 0;
    return k_ra8_ok;
}
