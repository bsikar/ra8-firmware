//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The SPH0690 filter policy: the coefficients, the per-microphone clock
//! edge, and the struct layout the C callers read it back through.

const std = @import("std");
const pdm_mic = @import("pdm_mic");

const Err = struct {
    const ok: u32 = 0;
    const invalid_arg: u32 = 0x103;
};

const mic1: u8 = 0;
const mic2: u8 = 1;

fn config(microphone: u8) pdm_mic.Config {
    var out: pdm_mic.Config = undefined;
    const err = pdm_mic.get(microphone, &out);
    std.debug.assert(err == Err.ok);
    return out;
}

test "MIC1 is accepted" {
    var out: pdm_mic.Config = undefined;
    try std.testing.expectEqual(Err.ok, pdm_mic.get(mic1, &out));
}

test "MIC2 is accepted" {
    var out: pdm_mic.Config = undefined;
    try std.testing.expectEqual(Err.ok, pdm_mic.get(mic2, &out));
}

test "a microphone past the last one is refused" {
    var out: pdm_mic.Config = undefined;
    try std.testing.expectEqual(Err.invalid_arg, pdm_mic.get(2, &out));
    try std.testing.expectEqual(Err.invalid_arg, pdm_mic.get(255, &out));
}

test "a refused microphone leaves the output alone" {
    var out: pdm_mic.Config = undefined;
    out.sample_rate_hz = 0xDEAD;
    _ = pdm_mic.get(2, &out);
    try std.testing.expectEqual(@as(u32, 0xDEAD), out.sample_rate_hz);
}

test "MIC1 reads on the rising edge" {
    try std.testing.expectEqual(@as(u8, 0), pdm_mic.edgeOf(mic1));
    try std.testing.expectEqual(@as(u8, 0), config(mic1).pdm.edge);
}

test "MIC2 reads on the falling edge" {
    try std.testing.expectEqual(@as(u8, 1), pdm_mic.edgeOf(mic2));
    try std.testing.expectEqual(@as(u8, 1), config(mic2).pdm.edge);
}

test "the edge is the only field the two microphones differ in" {
    var a = config(mic1);
    var b = config(mic2);
    a.pdm.edge = 0;
    b.pdm.edge = 0;
    try std.testing.expectEqualDeep(a, b);
}

test "the decimated rate is 16 kHz" {
    try std.testing.expectEqual(@as(u32, 16_000), config(mic1).sample_rate_hz);
}

test "the mics sit on PDM-IF channel 2" {
    try std.testing.expectEqual(@as(u8, 2), config(mic1).channel);
}

test "the payload is 20 significant bits" {
    try std.testing.expectEqual(@as(u8, 20), config(mic1).valid_bits);
}

test "the sinc filter runs at order 4" {
    try std.testing.expectEqual(@as(u8, 4), config(mic1).pdm.sinc_order);
}

test "the sinc decimation and clip range are the validated pair" {
    const cfg = config(mic1).pdm;
    try std.testing.expectEqual(@as(u8, 0x7C), cfg.sinc_dec);
    try std.testing.expectEqual(@as(u8, 0x05), cfg.sinc_range);
}

test "the clock divider is the reset value" {
    try std.testing.expectEqual(@as(u8, 0), config(mic1).pdm.clock_div);
}

test "every input shift is unshifted" {
    const cfg = config(mic1).pdm;
    try std.testing.expectEqual(@as(u8, 0), cfg.data_shift);
    try std.testing.expectEqual(@as(u8, 0), cfg.hpf_shift);
    try std.testing.expectEqual(@as(u8, 0), cfg.cf_shift);
    try std.testing.expectEqual(@as(u8, 0), cfg.lpf_shift);
}

test "the reception threshold is 4" {
    try std.testing.expectEqual(@as(u8, 4), config(mic1).pdm.rx_threshold);
}

test "the high-pass coefficients are the reset filter" {
    const cfg = config(mic1).pdm;
    try std.testing.expectEqual(@as(u16, 0x3F61), cfg.hpf_s0);
    try std.testing.expectEqual(@as(u16, 0x3EC1), cfg.hpf_k1);
    try std.testing.expectEqualSlices(u16, &.{ 0x4000, 0xC000 }, &cfg.hpf_h);
}

test "the compensation filter is HUM Table 49.7's row" {
    try std.testing.expectEqualSlices(u16, &.{
        0x1FE8, 0x0039, 0x003C, 0x1E56, 0x01DC, 0x06E1,
        0x01DC, 0x1E56, 0x003C, 0x0039, 0x1FE8,
    }, &config(mic1).pdm.comp_h);
}

test "the compensation filter is symmetric, as a linear-phase FIR must be" {
    const h = config(mic1).pdm.comp_h;
    for (0..h.len) |i| try std.testing.expectEqual(h[i], h[h.len - 1 - i]);
}

test "the low-pass gain and half-band taps are the validated set" {
    const cfg = config(mic1).pdm;
    try std.testing.expectEqual(@as(u16, 0x0400), cfg.lpf_h0);
    try std.testing.expectEqualSlices(u16, &.{
        0x1FF8, 0x000A, 0x1FF0, 0x0018, 0x1FDC, 0x0034, 0x1FB3,
        0x0076, 0x1F2E, 0x0289, 0x0289, 0x1F2E, 0x0076, 0x1FB3,
        0x0034, 0x1FDC, 0x0018, 0x1FF0, 0x000A, 0x1FF8,
    }, &cfg.lpf_h1);
}

test "the half-band taps are symmetric too" {
    const h = config(mic1).pdm.lpf_h1;
    for (0..h.len) |i| try std.testing.expectEqual(h[i], h[h.len - 1 - i]);
}

test "the coefficient arrays are the widths the HAL declares" {
    try std.testing.expectEqual(@as(usize, 2), pdm_mic.ChannelCfg.hpf_h_count);
    try std.testing.expectEqual(@as(usize, 11), pdm_mic.ChannelCfg.comp_h_count);
    try std.testing.expectEqual(@as(usize, 20), pdm_mic.ChannelCfg.lpf_h1_count);
}

test "the channel config is laid out as C declares it" {
    const C = pdm_mic.ChannelCfg;
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(C, "sinc_order"));
    try std.testing.expectEqual(@as(usize, 5), @offsetOf(C, "edge"));
    try std.testing.expectEqual(@as(usize, 9), @offsetOf(C, "rx_threshold"));
    try std.testing.expectEqual(@as(usize, 10), @offsetOf(C, "hpf_s0"));
    try std.testing.expectEqual(@as(usize, 12), @offsetOf(C, "hpf_k1"));
    try std.testing.expectEqual(@as(usize, 14), @offsetOf(C, "hpf_h"));
    try std.testing.expectEqual(@as(usize, 18), @offsetOf(C, "comp_h"));
    try std.testing.expectEqual(@as(usize, 40), @offsetOf(C, "lpf_h0"));
    try std.testing.expectEqual(@as(usize, 42), @offsetOf(C, "lpf_h1"));
    try std.testing.expectEqual(@as(usize, 2), @alignOf(C));
    try std.testing.expectEqual(@as(usize, 82), @sizeOf(C));
}

test "the microphone config is laid out as C declares it" {
    const M = pdm_mic.Config;
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(M, "pdm"));
    try std.testing.expectEqual(@as(usize, 84), @offsetOf(M, "sample_rate_hz"));
    try std.testing.expectEqual(@as(usize, 88), @offsetOf(M, "channel"));
    try std.testing.expectEqual(@as(usize, 89), @offsetOf(M, "valid_bits"));
    try std.testing.expectEqual(@as(usize, 4), @alignOf(M));
    try std.testing.expectEqual(@as(usize, 92), @sizeOf(M));
}

test "two reads of the same microphone agree" {
    try std.testing.expectEqualDeep(config(mic1), config(mic1));
}

test "reading MIC2 does not disturb a later read of MIC1" {
    _ = config(mic2);
    try std.testing.expectEqual(@as(u8, 0), config(mic1).pdm.edge);
}
