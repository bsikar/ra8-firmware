//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Secure-side entropy read behind the `ra8_nsc_trng_read` veneer.
//!
//! Port of the former `src/secure_trng.c`. The body is a xorshift64* core: a
//! DETERMINISTIC PRNG standing in for the RSIP TRNG that is not wired yet.
//! Predictable "random" bytes were the top finding in the security audit, so
//! the stand-in only exists in an off-target or explicitly-insecure image
//! (issue #180). A production image compiles the fail-closed arm, where every
//! entry point hard-errors and no predictable entropy can be drawn.

const std = @import("std");
const build_config = @import("build_config");

const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// Whether this image carries the PRNG stand-in at all.
pub const enabled = build_config.off_target or build_config.insecure_stub_crypto;

/// Sizing constants for a single read.
pub const Limits = struct {
    /// Max bytes per call.
    pub const max_bytes: u32 = 256;
};

/// Marsaglia's xorshift64* triple plus its output multiplier. The seed is the
/// golden-ratio constant.
const Tuning = struct {
    pub const seed: u64 = 0x9E3779B97F4A7C15;
    pub const multiplier: u64 = 0x2545F4914F6CDD1D;
    pub const shift_a: u6 = 12;
    pub const shift_b: u6 = 25;
    pub const shift_c: u6 = 27;
};

var state: u64 = Tuning.seed;

/// Advance the state and return one 64-bit word.
fn next() u64 {
    var x = state;
    x ^= x >> Tuning.shift_a;
    x ^= x << Tuning.shift_b;
    x ^= x >> Tuning.shift_c;
    state = x;
    return x *% Tuning.multiplier;
}

/// Re-seed to the boot default so reads are reproducible between test scenarios.
pub fn reset() Err {
    if (!enabled) return .not_supported;
    state = Tuning.seed;
    return .ok;
}

/// Fill `out` with entropy bytes.
///
/// `out` must already be secure scratch: the veneer copies from here to the NS
/// destination, so this function never touches NS memory itself.
pub fn read(out: []u8) Err {
    if (!enabled) return .not_supported;
    if (out.len == 0 or out.len > Limits.max_bytes) return .invalid_arg;

    var written: usize = 0;
    while (written < out.len) {
        const word = next();
        const chunk = @min(@as(usize, 8), out.len - written);
        // Little-endian, matching the C's `word >> (b * 8)` byte fan-out.
        var packed_word: [8]u8 = undefined;
        std.mem.writeInt(u64, &packed_word, word, .little);
        @memcpy(out[written..][0..chunk], packed_word[0..chunk]);
        written += chunk;
    }
    return .ok;
}
