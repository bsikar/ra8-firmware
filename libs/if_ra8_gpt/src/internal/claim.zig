//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which port owns each GPT channel. The timer and PWM adapters drive the
//! same ten GPT32 channels, so a channel opened through one port reads busy
//! to the other until it is closed. Pure state, so it is the part with a host
//! Zig test; the ops over it stay covered by the C suites.

const Err = @import("err").Err;

/// GPT320..GPT329, the channel count both adapters expose.
pub const channel_count: u8 = 10;

/// `fw_gpt_ra8_owner_t`.
pub const Owner = enum(u8) {
    none = 0,
    timer = 1,
    pwm = 2,
};

var owners = [_]Owner{.none} ** channel_count;

/// Take a free channel for `owner`.
pub fn claim(channel: u8, owner: Owner) u16 {
    if (channel >= channel_count or owner == .none) return Err.invalid_arg;
    if (owners[channel] != .none) return Err.busy;
    owners[channel] = owner;
    return Err.ok;
}

/// Whether `owner` currently holds `channel`.
pub fn ownedBy(channel: u8, owner: Owner) bool {
    return channel < channel_count and owner != .none and owners[channel] == owner;
}

/// Free `channel` if, and only if, `owner` holds it.
pub fn release(channel: u8, owner: Owner) void {
    if (ownedBy(channel, owner)) owners[channel] = .none;
}

/// Free every channel. Test-only: the adapters have no global reset.
pub fn resetForTest() void {
    owners = [_]Owner{.none} ** channel_count;
}
