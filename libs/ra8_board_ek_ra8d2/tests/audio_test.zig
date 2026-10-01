//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DA7212 bring-up and the PCM write path.

const std = @import("std");
const audio = @import("audio");

const ok: u32 = 0;
const invalid_arg: u32 = 0x103;
const not_initialized: u32 = 0x10F;
const hw_init_failed: u32 = 0x201;
const hw_timeout: u32 = 0x203;
const gpio_conflict: u32 = 0x205;

var routed: usize = 0;
var route_fails: bool = false;
var ssie_init_calls: usize = 0;
var ssie_init_fails: bool = false;
var last_channel: u8 = 0xFF;
var write_words: u16 = 0;
var write_short_by: u16 = 0;
var write_err: u32 = ok;

fn reset() void {
    routed = 0;
    route_fails = false;
    ssie_init_calls = 0;
    ssie_init_fails = false;
    last_channel = 0xFF;
    write_words = 0;
    write_short_by = 0;
    write_err = ok;
    audio.forgetInit();
}

export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    _ = pin;
    _ = psel;
    _ = owner;
    if (route_fails) return gpio_conflict;
    routed += 1;
    return ok;
}

export fn ra8_ssie_init(channel: u8, cfg: *const anyopaque) u32 {
    _ = cfg;
    ssie_init_calls += 1;
    last_channel = channel;
    return if (ssie_init_fails) hw_init_failed else ok;
}

export fn ra8_ssie_write_buffer(channel: u8, data: [*]const u32, len: u16, out_written: *u16) u32 {
    _ = data;
    last_channel = channel;
    write_words = len;
    out_written.* = len - write_short_by;
    return write_err;
}

fn initStereo16() u32 {
    return audio.init(48_000, 16, 2);
}

test "a stereo 16-bit link comes up" {
    reset();
    try std.testing.expectEqual(ok, initStereo16());
}

test "bring-up routes all six DA7212 pins" {
    reset();
    _ = initStereo16();
    try std.testing.expectEqual(@as(usize, 6), routed);
}

test "bring-up uses SSIE channel 0" {
    reset();
    _ = initStereo16();
    try std.testing.expectEqual(@as(u8, 0), last_channel);
}

test "a zero sample rate is refused before anything is routed" {
    reset();
    try std.testing.expectEqual(invalid_arg, audio.init(0, 16, 2));
    try std.testing.expectEqual(@as(usize, 0), routed);
}

test "only mono and stereo are accepted" {
    reset();
    try std.testing.expectEqual(ok, audio.init(48_000, 16, 1));
    reset();
    try std.testing.expectEqual(invalid_arg, audio.init(48_000, 16, 0));
    try std.testing.expectEqual(invalid_arg, audio.init(48_000, 16, 3));
    try std.testing.expectEqual(invalid_arg, audio.init(48_000, 16, 8));
}

test "an unmappable bit depth is refused before anything is routed" {
    reset();
    try std.testing.expectEqual(invalid_arg, audio.init(48_000, 17, 2));
    try std.testing.expectEqual(@as(usize, 0), routed);
}

test "a pin conflict aborts bring-up before SSIE is touched" {
    reset();
    route_fails = true;
    try std.testing.expectEqual(gpio_conflict, initStereo16());
    try std.testing.expectEqual(@as(usize, 0), ssie_init_calls);
}

test "an SSIE failure is reported as a hardware init failure" {
    reset();
    ssie_init_fails = true;
    try std.testing.expectEqual(hw_init_failed, initStereo16());
}

test "a failed bring-up does not count as initialized" {
    reset();
    ssie_init_fails = true;
    _ = initStereo16();
    try std.testing.expect(!audio.isInitialized());
}

test "a clean bring-up records the init" {
    reset();
    _ = initStereo16();
    try std.testing.expect(audio.isInitialized());
}

test "mono selects the monaural format, stereo selects I2S" {
    try std.testing.expectEqual(@as(u32, 3), audio.configFor(1, 16).?.format);
    try std.testing.expectEqual(@as(u32, 0), audio.configFor(2, 16).?.format);
}

test "the link is always an SSIE controller with AUCKE on" {
    const cfg = audio.configFor(2, 16).?;
    try std.testing.expectEqual(@as(u32, 1), cfg.role);
    try std.testing.expect(cfg.enable_aucke);
}

test "the bit-clock divider is 4, for 3.072 MHz from a 12.288 MHz master" {
    try std.testing.expectEqual(@as(u32, 2), audio.configFor(2, 32).?.bclk_div);
}

test "the config carries the word pair the depth maps to" {
    const cfg = audio.configFor(2, 24).?;
    try std.testing.expectEqual(@as(u32, 5), cfg.data_word);
    try std.testing.expectEqual(@as(u32, 2), cfg.system_word);
}

test "every optional SSIE behaviour is left off" {
    const cfg = audio.configFor(2, 16).?;
    try std.testing.expect(!cfg.use_gpt_clk);
    try std.testing.expect(!cfg.long_frame);
    try std.testing.expect(!cfg.byte_swap);
    try std.testing.expect(!cfg.lr_continue);
    try std.testing.expect(!cfg.bck_idle_stop);
    try std.testing.expectEqual(@as(u8, 0), cfg.tx_threshold);
    try std.testing.expectEqual(@as(u8, 0), cfg.rx_threshold);
}

test "playing before bring-up is refused" {
    reset();
    const pcm = [_]i16{ 1, 2, 3, 4 };
    try std.testing.expectEqual(not_initialized, audio.playSampleBlock(&pcm, pcm.len));
}

test "a null buffer is refused" {
    reset();
    _ = initStereo16();
    try std.testing.expectEqual(invalid_arg, audio.playSampleBlock(null, 4));
}

test "an empty block is refused" {
    reset();
    _ = initStereo16();
    const pcm = [_]i16{ 1, 2 };
    try std.testing.expectEqual(invalid_arg, audio.playSampleBlock(&pcm, 0));
}

test "an odd sample count cannot pack into 32-bit words" {
    reset();
    _ = initStereo16();
    const pcm = [_]i16{ 1, 2, 3 };
    try std.testing.expectEqual(invalid_arg, audio.playSampleBlock(&pcm, 3));
}

test "the argument checks run before the init check" {
    reset();
    try std.testing.expectEqual(invalid_arg, audio.playSampleBlock(null, 4));
}

test "a well-formed block is written" {
    reset();
    _ = initStereo16();
    const pcm align(4) = [_]i16{ 1, 2, 3, 4, 5, 6, 7, 8 };
    try std.testing.expectEqual(ok, audio.playSampleBlock(&pcm, pcm.len));
}

test "two samples pack into one FIFO word" {
    reset();
    _ = initStereo16();
    const pcm align(4) = [_]i16{ 1, 2, 3, 4, 5, 6, 7, 8 };
    _ = audio.playSampleBlock(&pcm, pcm.len);
    try std.testing.expectEqual(@as(u16, 4), write_words);
}

test "a short write is reported as a timeout" {
    reset();
    _ = initStereo16();
    write_short_by = 1;
    const pcm align(4) = [_]i16{ 1, 2, 3, 4 };
    try std.testing.expectEqual(hw_timeout, audio.playSampleBlock(&pcm, pcm.len));
}

test "a driver error is passed through unchanged" {
    reset();
    _ = initStereo16();
    write_err = 0x777;
    const pcm align(4) = [_]i16{ 1, 2, 3, 4 };
    try std.testing.expectEqual(@as(u32, 0x777), audio.playSampleBlock(&pcm, pcm.len));
}
