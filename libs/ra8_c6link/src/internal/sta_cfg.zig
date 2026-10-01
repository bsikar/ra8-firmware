//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The rules one set of station credentials is held to, both when it is
//! built and again when a join is asked for.
//!
//! Nothing here copies credentials: the C side owns the caller's buffers and
//! the codec's writable copies. This module only measures and judges, so the
//! same bounds decide a refusal in both places.

/// Bounds the 802.11 station credentials are held to.
pub const Bound = struct {
    /// Octets in the longest SSID 802.11 allows.
    pub const ssid_max: u8 = 32;
    /// Octets in the longest WPA passphrase.
    pub const pass_max: u8 = 64;
};

/// Why one set of credentials was refused.
pub const Refusal = error{
    InvalidSize,
};

/// Measure a string that is not trusted to be terminated.
///
/// Bounded by the buffer rather than by the string, so an unterminated one
/// measures as the whole buffer and the caller refuses it on length instead
/// of reading past its end.
pub fn length(text: []const u8) u8 {
    for (text, 0..) |byte, i| {
        if (byte == 0) return @intCast(i);
    }
    return @intCast(text.len);
}

/// Are these the lengths of one joinable network?
///
/// An empty SSID names no network, and an over-long one is either a caller's
/// mistake or a string that never terminated inside its buffer. A zero
/// passphrase is the open network, which is allowed.
pub fn credentialsValid(ssid_len: u8, pass_len: u8) Refusal!void {
    if (ssid_len == 0) return Refusal.InvalidSize;
    if (ssid_len > Bound.ssid_max) return Refusal.InvalidSize;
    if (pass_len > Bound.pass_max) return Refusal.InvalidSize;
}
