//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! PWM delay generation circuit (PDG) logic (RA8FW-799), ported from
//! ra8_pdg.c. HUM Ch 23. The register sequences are generic over a Hw
//! with read16/write16 plus the two waits, so tests drive a fake.

const std = @import("std");

pub const codes = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const not_initialized: u16 = 0x10F;
    pub const out_of_range: u16 = 0x208;
    pub const null_ptr: u16 = 0x504;
};

const ok = codes.ok;
const invalid_arg = codes.invalid_arg;

/// PDG Secure alias and its registers (HUM 23.2, p 1154-1158).
pub const base: usize = 0x4032_4000;
pub const gtdlycr: usize = base + 0x000;
pub const gtdlycr2: usize = base + 0x002;
const rise_off: usize = 0x018;
const fall_off: usize = 0x028;

/// GPT channel n's GTWP (+0x00) lives at gpt0_base + n * gpt_stride.
pub const gpt0_base: usize = 0x4032_2000;
pub const gpt_stride: usize = 0x100;
pub const gtwp_unlock: u32 = 0xA500;
pub const gtwp_lock: u32 = 0xA501;
/// MSTPD6 (k_ra8_mstp_pdg).
pub const mstp_pdg: u16 = (3 << 8) | 6;

pub const channel_count: u8 = 4;
pub const slot_count: u8 = 16;
const channel_mask_all: u8 = 0x0F;

pub const dllen: u16 = 0x0001;
pub const dlyrst: u16 = 0x0002;
const frange_mask: u16 = 0x0300;
pub const dly_mask: u16 = 0x007F;
pub const dly_max: u8 = 0x7F;
pub const status_in_reset: u16 = 0x0002;

pub const frange_low: u8 = 0; // 80..160 MHz, 1/128 step
pub const frange_high: u8 = 1; // 155..300 MHz, 1/64 step
const freq_low_min: u32 = 80_000_000;
const freq_high_max: u32 = 300_000_000;
const freq_overlap_top: u32 = 160_000_000;
const ns_per_sec: u32 = 1_000_000_000;

/// Spin counts: 20 us DLL lock at 1000 nops per us, 32 nops for >= 5 GTCLK.
pub const dll_lock_us: u16 = 20;
pub const loops_per_us: u16 = 1000;
pub const post_reset_loops: u16 = 32;

pub const pin_a: u8 = 0;
pub const pin_b: u8 = 1;
pub const edge_rising: u8 = 0;
pub const edge_falling: u8 = 1;
pub const wave_saw: u8 = 0;
pub const wave_triangle: u8 = 1;
pub const dir_up: u8 = 0;
pub const dir_down: u8 = 1;

/// ra8_pdg_config_t.
pub const Config = extern struct {
    frange: u8,
    channel_mask: u8,
    auto_tune: u8,
    gptclk_hz: u32,
};

/// ra8_pdg_status_full_t.
pub const StatusFull = extern struct {
    dll_enabled: u8,
    in_reset: u8,
    frange: u8,
    per_channel_bypass_off: [channel_count]u8,
    per_channel_powered: [channel_count]u8,
    raw_gtdlycr: u16,
    raw_gtdlycr2: u16,
};

/// ra8_pdg_delay_entry_t.
pub const DelayEntry = extern struct {
    channel: u8,
    pin: u8,
    edge: u8,
    code: u8,
};

comptime {
    std.debug.assert(@sizeOf(Config) == 8 and @offsetOf(Config, "gptclk_hz") == 4);
    std.debug.assert(@sizeOf(StatusFull) == 16 and @offsetOf(StatusFull, "raw_gtdlycr") == 12);
    std.debug.assert(@sizeOf(DelayEntry) == 4);
}

/// DLLEN set and DLYRST clear.
pub fn isInitialized(cr: u16) bool {
    return cr & dllen != 0 and cr & dlyrst == 0;
}

pub fn frangeOk(f: u8) bool {
    return f == frange_low or f == frange_high;
}

fn frangeBits(f: u8) u16 {
    return @as(u16, f) << 8;
}

pub fn validateCfg(c: Config) u16 {
    if (c.channel_mask & ~channel_mask_all != 0) return invalid_arg;
    if (c.auto_tune != 0) {
        if (c.gptclk_hz == 0) return invalid_arg;
        if (c.gptclk_hz < freq_low_min or c.gptclk_hz > freq_high_max) return codes.out_of_range;
        return ok;
    }
    return if (frangeOk(c.frange)) ok else invalid_arg;
}

pub const Pick = struct { rc: u16, frange: u8 = frange_low };

/// The overlap 155..160 MHz goes to the low band.
pub fn pickFrange(hz: u32) Pick {
    if (hz == 0) return .{ .rc = invalid_arg };
    if (hz < freq_low_min or hz > freq_high_max) return .{ .rc = codes.out_of_range };
    return .{ .rc = ok, .frange = if (hz <= freq_overlap_top) frange_low else frange_high };
}

pub fn slotOk(ch: u8, pin: u8, edge: u8, code: u8) u16 {
    if (ch >= channel_count) return invalid_arg;
    if (pin != pin_a and pin != pin_b) return invalid_arg;
    if (edge != edge_rising and edge != edge_falling) return invalid_arg;
    if (code > dly_max) return invalid_arg;
    return ok;
}

/// GTDLYRnA/B (rising) or GTDLYFnA/B (falling).
pub fn cellAddr(ch: u8, pin: u8, edge: u8) usize {
    const off = if (edge == edge_rising) rise_off else fall_off;
    const b: usize = if (pin == pin_a) 0 else 2;
    return base + off + @as(usize, ch) * 4 + b;
}

/// Round half up, clamped to DLY max; 64-bit wrapping like the C.
pub fn nsToCode(delay_ns: u32, hz: u32, frange: u8) u8 {
    const div: u64 = if (frange == frange_low) 128 else 64;
    const numer = @as(u64, delay_ns) *% div *% hz;
    const code = (numer +% ns_per_sec / 2) / ns_per_sec;
    return @intCast(@min(code, dly_max));
}

pub fn dlybsBit(ch: u8) u16 {
    return @as(u16, 1) << @intCast(ch);
}

pub fn dlyenBit(ch: u8) u16 {
    return @as(u16, 1) << @intCast(8 + @as(u5, @intCast(ch)));
}

pub fn decodeStatus(cr: u16, cr2: u16) StatusFull {
    var s = StatusFull{
        .dll_enabled = @intFromBool(cr & dllen != 0),
        .in_reset = @intFromBool(cr & dlyrst != 0),
        .frange = @intCast((cr & frange_mask) >> 8),
        .per_channel_bypass_off = undefined,
        .per_channel_powered = undefined,
        .raw_gtdlycr = cr,
        .raw_gtdlycr2 = cr2,
    };
    for (0..channel_count) |i| {
        const ch: u8 = @intCast(i);
        s.per_channel_bypass_off[i] = @intFromBool(cr2 & dlybsBit(ch) != 0);
        s.per_channel_powered[i] = @intFromBool(cr2 & dlyenBit(ch) == 0); // DLYEN inverted
    }
    return s;
}

/// HUM Table 23.4: compare-match limits per wave mode and direction.
pub fn checkConstraints(mode: u8, dir: u8, compare_match: u32, gtpr: u32) u16 {
    if (mode != wave_saw and mode != wave_triangle) return invalid_arg;
    if (dir != dir_up and dir != dir_down) return invalid_arg;
    if (mode == wave_saw and dir == dir_up) {
        if (gtpr < 2) return codes.invalid_state;
        return if (compare_match >= gtpr - 2) codes.invalid_state else ok;
    }
    if (dir == dir_down and compare_match <= 2) return codes.invalid_state;
    return ok;
}

/// PCLKA period x 6 plus GPTCLK period x 4, u32 wrapping like the C.
pub fn requiredWriteNs(pclka_hz: u32, gptclk_hz: u32) u32 {
    return (ns_per_sec / pclka_hz) *% 6 +% (ns_per_sec / gptclk_hz) *% 4;
}

/// HUM Figure 23.2 (p 1160) DLL bring-up, then bypass off for `mask`.
pub fn programDll(hw: anytype, mask: u8, frange: u8) void {
    const bits = frangeBits(frange);
    hw.write16(gtdlycr, dlyrst | bits);
    hw.write16(gtdlycr2, 0);
    hw.write16(gtdlycr, dlyrst | dllen | bits);
    hw.waitUs(dll_lock_us);
    hw.write16(gtdlycr, dllen | bits);
    hw.wait5Gtclk();
    hw.write16(gtdlycr2, mask);
}

/// FRANGE may only change with DLLEN = 0; GTDLYCR2 is saved and restored.
pub fn switchFrange(hw: anytype, frange: u8) void {
    const saved = hw.read16(gtdlycr2);
    hw.write16(gtdlycr, dlyrst);
    hw.write16(gtdlycr2, 0);
    const bits = frangeBits(frange);
    hw.write16(gtdlycr, dlyrst | bits);
    hw.write16(gtdlycr, dlyrst | dllen | bits);
    hw.waitUs(dll_lock_us);
    hw.write16(gtdlycr, dllen | bits);
    hw.wait5Gtclk();
    hw.write16(gtdlycr2, saved);
}

/// Deinit: DLLEN 0, DLYRST 1, FRANGE 0, then every delay cell to 0.
pub fn park(hw: anytype) void {
    hw.write16(gtdlycr, dlyrst);
    hw.write16(gtdlycr2, 0);
    for (0..channel_count) |i| {
        const ch: u8 = @intCast(i);
        hw.write16(cellAddr(ch, pin_a, edge_rising), 0);
        hw.write16(cellAddr(ch, pin_b, edge_rising), 0);
        hw.write16(cellAddr(ch, pin_a, edge_falling), 0);
        hw.write16(cellAddr(ch, pin_b, edge_falling), 0);
    }
}

/// Read-modify-write one GTDLYCR2 bit.
pub fn setCr2Bit(hw: anytype, bit: u16, on: bool) void {
    const v = hw.read16(gtdlycr2);
    hw.write16(gtdlycr2, if (on) v | bit else v & ~bit);
}
