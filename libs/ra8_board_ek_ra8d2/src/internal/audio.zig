//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The DA7212 audio link: bring SSIE0 up in I2S controller mode, then feed
//! it PCM.

const audio_pins = @import("audio_pins.zig");
const audio_word = @import("audio_word.zig");
const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Audio = vocab.Audio;
const Err = vocab.Err;

/// SSIE roles and formats, HUM SSICR.
const Role = struct {
    const controller: u32 = 1;
};
const Format = struct {
    const i2s: u32 = 0;
    const monaural: u32 = 3;
};
/// AUDIO_MCK / 4 lands near 12.288 MHz / 4 = 3.072 MHz BCK, which is 48 kHz
/// at two channels of 32-bit frames. Finer rate matching is left to an
/// application that re-initialises SSIE with a tuned divider.
const bclk_div_4: u32 = 2;

var initialized: bool = false;

/// Test seam: whether a successful init has been recorded.
pub fn isInitialized() bool {
    return initialized;
}

/// Test seam: forget the init, so a suite can exercise the not-initialized path.
pub fn forgetInit() void {
    initialized = false;
}

fn buildCfg(channels: u8, words: audio_word.Words) hal.SsieCfg {
    return .{
        .role = Role.controller,
        .format = if (channels == Audio.channels_mono) Format.monaural else Format.i2s,
        .data_word = words.data,
        .system_word = words.system,
        .bclk_div = bclk_div_4,
        .use_gpt_clk = false,
        .long_frame = false,
        .bckp_rising = false,
        .lrckp_low = false,
        .spdp_high = false,
        .byte_swap = false,
        .lr_continue = false,
        .bck_idle_stop = false,
        .enable_aucke = true,
        .tx_threshold = 0,
        .rx_threshold = 0,
    };
}

/// Test seam: the config the given frame shape produces.
pub fn configFor(channels: u8, bit_depth: u8) ?hal.SsieCfg {
    const words = audio_word.forBits(bit_depth) orelse return null;
    return buildCfg(channels, words);
}

/// Route the DAI pins, then bring SSIE0 up. `sample_rate_hz` is validated but
/// not yet programmed: the divider above fixes the frame rate.
pub fn init(sample_rate_hz: u32, bit_depth: u8, channels: u8) u32 {
    if (sample_rate_hz == 0) return Err.invalid_arg;
    if (channels != Audio.channels_mono and channels != Audio.channels_stereo) {
        return Err.invalid_arg;
    }
    const words = audio_word.forBits(bit_depth) orelse return Err.invalid_arg;

    const route_err = audio_pins.routeAll();
    if (route_err != Err.ok) return route_err;

    const cfg = buildCfg(channels, words);
    if (hal.ra8_ssie_init(Audio.ssie_channel, &cfg) != Err.ok) return Err.hw_init_failed;
    initialized = true;
    return Err.ok;
}

/// Push one block of PCM at the SSIE FIFO. Two int16 samples pack into one
/// 32-bit word, so `len` has to be even; the caller's buffer is 32-bit
/// aligned, which one stereo frame satisfies naturally.
pub fn playSampleBlock(buf: ?[*]const i16, len: u32) u32 {
    const samples = buf orelse return Err.invalid_arg;
    if (len == 0 or len % Audio.samples_per_word != 0) return Err.invalid_arg;
    if (!initialized) return Err.not_initialized;

    const words: u16 = @intCast(len / Audio.samples_per_word);
    const packed_words: [*]const u32 = @ptrCast(@alignCast(samples));
    var written: u16 = 0;
    const err = hal.ra8_ssie_write_buffer(Audio.ssie_channel, packed_words, words, &written);
    if (err != Err.ok) return err;
    if (written != words) return Err.hw_timeout;
    return Err.ok;
}
