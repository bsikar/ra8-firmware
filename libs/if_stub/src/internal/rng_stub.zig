//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The stub entropy profile, and nothing else.
//!
//! `if_stub` is the proof binding of epic RA8FW-618 slice 5: the `if_<chip>`
//! set a second target supplies in place of the RA8 adapters. This unit is
//! that set's random-bytes member. It reads no hardware and touches no
//! register, so the whole profile is a pure function of its call history and
//! runs on the host without a fake peripheral block.
//!
//! The bytes are deterministic, not entropy: fill N hands back byte i as
//! (i + N) mod 256. A caller can tell consecutive fills apart, which is what
//! exercising the port's fill and ready intents needs, and the header says in
//! as many words that these bytes are never key material. A real chip port
//! replaces this file, and the interface above it never changes.

/// How many fills this profile has served since reset. Increments once per
/// fill, which is what makes consecutive fills differ.
var fill_count: usize = 0;

/// Fill `out` with the stub pattern. Deterministic, not entropy. A
/// zero-length fill is satisfied without touching the profile's state, the
/// way the port's own contract describes the request.
pub fn fill(out: []u8) void {
    if (out.len == 0) return;

    for (out, 0..) |*byte, i| {
        byte.* = @truncate(i +% fill_count);
    }

    fill_count +%= 1;
}

/// The stub source always serves. The port's `fw_rng_ready` is a report, not
/// a reservation, and this profile never has contention to report.
pub fn ready() bool {
    return true;
}

/// How many fills the profile has served; tests use it to assert that a
/// zero-length fill costs nothing.
pub fn fills() usize {
    return fill_count;
}

/// Restore fill zero. Test hygiene, not product surface.
pub fn reset() void {
    fill_count = 0;
}
