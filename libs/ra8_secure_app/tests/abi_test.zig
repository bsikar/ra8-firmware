//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C membrane screens some arguments before the vault sees them, so the
//! order of those screens is part of what a caller observes. `build.zig` links
//! this root against an archive built for each side of the fail-closed split,
//! and `vault` is the same source under the same options, so `vault.enabled`
//! says which side this binary is on. A production image must answer
//! `not_supported` from every vault entry point to every call made with valid
//! pointers.

const std = @import("std");
const vault = @import("vault");

extern fn ra8_key_vault_set_mac_key(key: ?[*]const u8, key_len: u16) u16;

const key: [vault.Limits.mac_key_256]u8 = @splat(0x5A);

/// The 192-bit length `secure_app_vault_kat` sends: neither AES-128 nor AES-256.
const bad_len: u16 = 24;

test "set_mac_key with a bad length reports the vault's state first" {
    const expected: vault.Err = if (vault.enabled) .invalid_arg else .not_supported;
    try std.testing.expectEqual(expected.code(), ra8_key_vault_set_mac_key(&key, bad_len));
}

test "set_mac_key with a good length reaches the vault" {
    const expected: vault.Err = if (vault.enabled) .ok else .not_supported;
    try std.testing.expectEqual(
        expected.code(),
        ra8_key_vault_set_mac_key(&key, vault.Limits.mac_key_128),
    );
}
