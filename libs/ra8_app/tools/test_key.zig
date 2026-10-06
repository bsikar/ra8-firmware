//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The test-only signing identity the build packs txm_hello_m33.ra8app with
//! (RA8FW-479). The seed is public on purpose: an image signed with it proves
//! the pack-and-verify path, never provenance, and no device key derives from
//! it. The build writes the seed to a file for `ra8app_pack`, and the check
//! derives the same public key from it.

/// The raw Ed25519 seed, the same fill module_pack_test.zig signs with.
pub const seed: [32]u8 = @splat(0x24);
/// The manifest identity of the hello-world module.
pub const app_id = "com.ra8.txm_hello_m33";
pub const display_name = "Hello M33";
/// No capabilities: the hello-world module only prints.
pub const capabilities = "0";
