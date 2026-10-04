//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CAN FD bit-timing solver and NCFG/DCFG packing (HUM Ch 41). Pure
//! code; the C ABI and register access live in canfd_timing_abi.zig
//! (RA8FW-580).

/// CANFD channel bases (`k_ra8_canfd0_base_addr`, `k_ra8_canfd1_base_addr`).
pub const channel_bases = [_]usize{ 0x4038_0000, 0x4038_2000 };
/// CFDC[0].NCFG and CFDC2[0].DCFG offsets from the channel base.
pub const off_ncfg: usize = 0x000;
pub const off_dcfg: usize = 0x100;

/// Nominal phase: 10-bit prescaler. Data phase: 8-bit prescaler.
pub const prescaler_max: u32 = 1024;
pub const data_prescaler_max: u32 = 256;
pub const sjw_max: u32 = 16;

const tq_hi: u32 = 25;
const tq_lo: u32 = 8;

/// Every field is the plain count, before the register's minus-one.
pub const Timing = struct { prescaler: u32, tseg1: u32, tseg2: u32, sjw: u32 };

pub fn channelBase(channel: u8) ?usize {
    if (channel >= channel_bases.len) return null;
    return channel_bases[channel];
}

/// Largest exact time-quanta count from 25 down to 8 with a 75% sample
/// point; null when no count divides the clock within the prescaler cap.
/// A wrapped-to-zero denominator (C UB) skips that count.
pub fn solve(clock_hz: u32, bitrate_bps: u32, max_prescaler: u32) ?Timing {
    if (bitrate_bps == 0 or clock_hz == 0) return null;
    var tq: u32 = tq_hi;
    while (tq >= tq_lo) : (tq -= 1) {
        const denom = bitrate_bps *% tq;
        if (denom == 0 or clock_hz % denom != 0) continue;
        const prescaler = clock_hz / denom;
        if (prescaler > max_prescaler) continue;
        const tseg1 = ((tq - 1) * 3) / 4;
        const tseg2 = (tq - 1) - tseg1;
        return .{ .prescaler = prescaler, .tseg1 = tseg1, .tseg2 = tseg2, .sjw = @min(tseg2, sjw_max) };
    }
    return null;
}

pub fn packNcfg(t: Timing) u32 {
    return ((t.prescaler -% 1) & 0x3FF) |
        (((t.sjw -% 1) & 0x7F) << 10) |
        ((t.tseg1 & 0xFF) << 17) |
        ((t.tseg2 & 0x7F) << 25);
}

pub fn packDcfg(t: Timing) u32 {
    return ((t.prescaler -% 1) & 0xFF) |
        ((t.tseg1 & 0x1F) << 8) |
        ((t.tseg2 & 0xF) << 16) |
        (((t.sjw -% 1) & 0xF) << 24);
}
