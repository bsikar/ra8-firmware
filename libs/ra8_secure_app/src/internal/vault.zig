//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Secure-only symmetric key store: eight 256-bit slots plus the separate
//! key-authentication key (KAK) that `key_import.c` uses to key its CMAC.
//!
//! Port of the former `src/key_vault.c`. Two behaviours are load-bearing and
//! are preserved exactly:
//!
//! * The raw key never leaves the secure world. The only operation the NS side
//!   can reach (through the `ra8_nsc_key_vault_challenge` veneer) is
//!   `sha256XorChallenge`, which returns SHA-256(key XOR challenge).
//! * The fail-closed split from the crypto gate. The vault body only exists in an
//!   off-target or explicitly-insecure image; a production image serves
//!   `not_supported` from every entry point rather than operate a vault whose
//!   hardened storage backend is not wired yet. That switch is a comptime
//!   constant inside this one module, not a second file.
//!
//! The C hand-rolled a single-block SHA-256 here. This port calls
//! `std.crypto.hash.sha2.Sha256` instead: the two agree byte-for-byte (the
//! known-answer vectors in `tests/vault_test.zig` were taken from the C and
//! independently confirmed), and dropping ~200 lines of open-coded schedule and
//! compression removes a hand-rolled crypto primitive from the tree.

const std = @import("std");
const build_config = @import("build_config");

const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// Whether this image carries the vault body at all.
///
/// The C spelled this `#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)`.
pub const enabled = build_config.off_target or build_config.insecure_stub_crypto;

/// Sizing constants for the vault.
pub const Limits = struct {
    /// Number of stored keys.
    pub const slots: u16 = 8;
    /// 256-bit symmetric key.
    pub const key_bytes: u16 = 32;
    /// Challenge length.
    pub const chal_bytes: u16 = 32;
    /// SHA-256 output size.
    pub const digest_bytes: u16 = 32;
    /// Capacity of the KAK store.
    pub const mac_key_bytes: u16 = 32;
    /// AES-128 KAK length.
    pub const mac_key_128: u16 = 16;
    /// AES-256 KAK length.
    pub const mac_key_256: u16 = 32;
};

const Key = [Limits.key_bytes]u8;

var slots: [Limits.slots]Key = @splat(@splat(0));

/// The KAK lives in its own store, deliberately not in `slots`: nothing an NS
/// caller can reach through the import path may overwrite or read it.
var mac_key: [Limits.mac_key_bytes]u8 = @splat(0);
var mac_key_len: u16 = 0;

/// Zero every slot and drop the provisioned KAK.
pub fn init() Err {
    if (!enabled) return .not_supported;
    slots = @splat(@splat(0));
    mac_key = @splat(0);
    mac_key_len = 0;
    return .ok;
}

/// Programme a 256-bit symmetric key into `slot`.
pub fn store(slot: u16, key: *const Key) Err {
    if (!enabled) return .not_supported;
    if (slot >= Limits.slots) return .invalid_arg;
    slots[slot] = key.*;
    return .ok;
}

/// Compute SHA-256(key XOR challenge) for `slot`.
///
/// This is the only vault operation reachable from the Non-Secure world. The
/// digest depends on both the key and the challenge and reveals neither.
pub fn sha256XorChallenge(
    slot: u16,
    challenge: *const [Limits.chal_bytes]u8,
    out: *[Limits.digest_bytes]u8,
) Err {
    if (!enabled) return .not_supported;
    if (slot >= Limits.slots) return .invalid_arg;

    var scratch: Key = undefined;
    for (&scratch, slots[slot], challenge) |*dst, key_byte, chal_byte| {
        dst.* = key_byte ^ chal_byte;
    }
    std.crypto.hash.sha2.Sha256.hash(&scratch, out, .{});
    // Wipe the key-XOR-challenge value before the secure frame is reused.
    std.crypto.secureZero(u8, &scratch);
    return .ok;
}

/// Provision the key-authentication key used to MAC key imports.
pub fn setMacKey(key: []const u8) Err {
    if (!enabled) return .not_supported;
    const len: u16 = @intCast(key.len);
    if (len != Limits.mac_key_128 and len != Limits.mac_key_256) return .invalid_arg;
    mac_key = @splat(0);
    @memcpy(mac_key[0..key.len], key);
    mac_key_len = len;
    return .ok;
}

/// Copy the provisioned KAK into a secure caller's buffer.
///
/// Returns the number of bytes copied through `out_len`. The caller wipes its
/// copy after use; no veneer exposes this to NS.
pub fn loadMacKey(out: []u8, out_len: *u16) Err {
    if (!enabled) return .not_supported;
    if (mac_key_len == 0) return .not_found;
    if (out.len < mac_key_len) return .invalid_size;
    @memcpy(out[0..mac_key_len], mac_key[0..mac_key_len]);
    out_len.* = mac_key_len;
    return .ok;
}
