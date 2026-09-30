//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The deterministic `rand()` the firmware links instead of newlib's.
//!
//! Newlib heap-allocates per-thread PRNG state on the first call, which trips
//! `ra8_sbrk_trap`, and NetX Duo calls `rand()` for ISN, cookie and jitter
//! generation. So the firmware owes itself a working generator that never
//! allocates: one xorshift32 word, Marsaglia's 13/17/5 constants.
//!
//! NOT cryptographically secure. A caller needing a CSPRNG goes through the
//! `ra8_rsip` TRNG.

/// Marsaglia's xorshift32 shift triple, and the seed that replaces zero.
pub const xorshift = struct {
    pub const shift_a: u5 = 13;
    pub const shift_b: u5 = 17;
    pub const shift_c: u5 = 5;
    /// An all-zero state locks xorshift forever, so a zero seed takes this
    /// instead: an arbitrary non-zero constant, per Marsaglia 2003.
    pub const default_seed: u32 = 0x9E37_79B9;
};

/// `RAND_MAX`, pinned rather than read from a libc header.
///
/// The C masked with whichever `RAND_MAX` `<stdlib.h>` happened to define at
/// its compile site. Both toolchains in play say `0x7FFFFFFF`, so pinning it
/// keeps today's behaviour and stops a host header from quietly setting the
/// range an image returns.
pub const rand_max: u32 = 0x7FFF_FFFF;

var state: u32 = xorshift.default_seed;

/// Seed the generator, remapping zero to the default so it cannot lock.
pub fn seed(value: u32) void {
    state = if (value == 0) xorshift.default_seed else value;
}

/// Advance one xorshift32 step and return the masked result.
pub fn next() u32 {
    var x = state;
    x ^= x << xorshift.shift_a;
    x ^= x >> xorshift.shift_b;
    x ^= x << xorshift.shift_c;
    state = x;
    return x & rand_max;
}
