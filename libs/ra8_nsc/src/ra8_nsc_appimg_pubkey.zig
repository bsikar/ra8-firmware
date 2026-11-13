//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The secure-flash home of the pinned .ra8app verification key
//! (RA8FW-291). Admission verifies under the key the `.ra8_nsc_appimg_pubkey`
//! section of the Secure image's flash carries, so provisioning replaces the
//! key by writing that section and never touches code; the key never leaves
//! the Secure world, since admission reads it secure-side and hands only the
//! pointer to the verify backend. The shipped default is the key
//! libs/ra8_app/tools/test_key.zig's seed derives, so images `zig build
//! txm-hello-m33` signs keep passing until real provisioning lands. On-silicon
//! placement of the section inside the secure MRAM window is the
//! hardware-gated three-case proof (RA8FW-1019's lane).

const std = @import("std");

/// The Ed25519 verification key's width: 32 bytes.
pub const pubkey_bytes = 32;

/// The shipped default: the public key libs/ra8_app/tools/test_key.zig's
/// seed derives. Provisioning overwrites the section in place; this default
/// keeps the test-signing flow working until then.
pub const default_key = [pubkey_bytes]u8{
    0x58, 0x93, 0x66, 0x04, 0xab, 0xda, 0x11, 0x2b,
    0xc9, 0x49, 0x33, 0x56, 0x9c, 0x82, 0xf8, 0xd0,
    0xcc, 0x0d, 0xdf, 0x92, 0xa3, 0xf8, 0x32, 0x9f,
    0x2f, 0x44, 0x8f, 0x7f, 0x48, 0x4a, 0x59, 0x4c,
};

/// The pinned key as flash carries it: its own read-only section in the
/// Secure image, so a provisioning pass rewrites the section in place and
/// nothing else in the image has to move.
export const k_ra8_nsc_appimg_pubkey linksection(".ra8_nsc_appimg_pubkey") = default_key;

/// The key admission verifies under: the flash-resident section, read
/// secure-side only.
pub fn pinned() *const [pubkey_bytes]u8 {
    return &k_ra8_nsc_appimg_pubkey;
}

test "pinned returns the flash section's key" {
    try std.testing.expectEqual(@intFromPtr(&k_ra8_nsc_appimg_pubkey), @intFromPtr(pinned()));
    try std.testing.expectEqual(pubkey_bytes, pinned().len);
    try std.testing.expectEqualSlices(u8, &default_key, pinned());
}
