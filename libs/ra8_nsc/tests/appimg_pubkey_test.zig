//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host unit tests for `ra8_nsc_appimg_pubkey`.

const std = @import("std");
const pubkey = @import("pubkey");

test "pinned returns the flash section's key" {
    try std.testing.expectEqual(@intFromPtr(&pubkey.k_ra8_nsc_appimg_pubkey), @intFromPtr(pubkey.pinned()));
    try std.testing.expectEqual(pubkey.pubkey_bytes, pubkey.pinned().len);
    try std.testing.expectEqualSlices(u8, &pubkey.default_key, pubkey.pinned());
}
