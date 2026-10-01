//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The SPH0690 filter policy: the validated PDM-IF coefficients for the
//! board's two microphones, and which clock edge each one is read on.
//!
//! Coefficients are HUM Table 49.7's row plus the reset filter values. They
//! are the board's measured defaults, not a derivation, so they are a table
//! rather than a computation.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Pdm = vocab.Pdm;

pub const ChannelCfg = hal.PdmChannelCfg;
pub const Config = hal.PdmMicConfig;

/// The SPH0690 as validated on this board. `edge` is filled per microphone by
/// `get`, since it is the one field the two mics differ in.
const sph0690: ChannelCfg = .{
    .sinc_order = 4,
    .clock_div = 0,
    .sinc_dec = 0x7C,
    .sinc_range = 0x05,
    .data_shift = 0,
    .edge = Pdm.edge_rising,
    .hpf_shift = 0,
    .cf_shift = 0,
    .lpf_shift = 0,
    .rx_threshold = 4,
    .hpf_s0 = 0x3F61,
    .hpf_k1 = 0x3EC1,
    .hpf_h = .{ 0x4000, 0xC000 },
    .comp_h = .{
        0x1FE8, 0x0039, 0x003C, 0x1E56, 0x01DC, 0x06E1,
        0x01DC, 0x1E56, 0x003C, 0x0039, 0x1FE8,
    },
    .lpf_h0 = 0x0400,
    .lpf_h1 = .{
        0x1FF8, 0x000A, 0x1FF0, 0x0018, 0x1FDC, 0x0034, 0x1FB3,
        0x0076, 0x1F2E, 0x0289, 0x0289, 0x1F2E, 0x0076, 0x1FB3,
        0x0034, 0x1FDC, 0x0018, 0x1FF0, 0x000A, 0x1FF8,
    },
};

/// MIC1 has SELECT tied low and is read on the rising edge; MIC2 is tied high
/// and is read on the falling edge of the channel below it.
fn edgeFor(microphone: u8) u8 {
    return if (microphone == Pdm.mic1) Pdm.edge_rising else Pdm.edge_falling;
}

/// The whole microphone story for one mic: filter policy, rate, channel and
/// payload width, so the caller configures the PDM-IF from one struct.
pub fn get(microphone: u8, out: *Config) u32 {
    if (microphone >= Pdm.mic_count) return Err.invalid_arg;

    out.* = .{
        .pdm = sph0690,
        .sample_rate_hz = Pdm.sample_rate_hz,
        .channel = Pdm.channel,
        .valid_bits = Pdm.valid_bits,
    };
    out.pdm.edge = edgeFor(microphone);
    return Err.ok;
}

/// Test seam: the edge policy on its own.
pub fn edgeOf(microphone: u8) u8 {
    return edgeFor(microphone);
}
