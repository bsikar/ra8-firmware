//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The signature half of txm_sd_hello_m85 (RA8FW-830): admit an .ra8app
//! through ra8_app's admission gate (appimg_verify.verify: container, grant,
//! then signature) with a software Ed25519 backend and the pinned test public
//! key. The key is the one libs/ra8_app/tools/test_key.zig's seed derives, so
//! only images `zig build txm-hello-m33` signs pass.

const std = @import("std");
const appimg_verify = @import("appimg_verify");

const Ed25519 = std.crypto.sign.Ed25519;

/// Ed25519 public key of test_key.zig's seed (0x24 repeated 32 times).
pub const public_key = [appimg_verify.pubkey_bytes]u8{
    0x58, 0x93, 0x66, 0x04, 0xab, 0xda, 0x11, 0x2b,
    0xc9, 0x49, 0x33, 0x56, 0x9c, 0x82, 0xf8, 0xd0,
    0xcc, 0x0d, 0xdf, 0x92, 0xa3, 0xf8, 0x32, 0x9f,
    0x2f, 0x44, 0x8f, 0x7f, 0x48, 0x4a, 0x59, 0x4c,
};
/// Capability bits this host grants: none; txm_hello_m33 asks for none.
pub const granted: u32 = 0;
/// sizeof(appimg.Header): the module payload starts here.
pub const header_bytes = 160;

fn ed25519(
    ctx: ?*anyopaque,
    msg: appimg_verify.Message,
    signature: *const [Ed25519.Signature.encoded_length]u8,
    key: *const [appimg_verify.pubkey_bytes]u8,
) appimg_verify.Verdict {
    _ = ctx;
    const sig = Ed25519.Signature.fromBytes(signature.*);
    const pk = Ed25519.PublicKey.fromBytes(key.*) catch return .bad;
    var v = sig.verifier(pk) catch return .bad;
    v.update(msg.head);
    v.update(msg.tail);
    v.verify() catch return .bad;
    return .good;
}

const backend = appimg_verify.Backend{ .verify = &ed25519, .public_key = &public_key, .granted = granted };

/// True when `bytes` is an .ra8app signed by the pinned key whose
/// capabilities fit `granted`.
pub fn admitted(bytes: []const u8) bool {
    _ = appimg_verify.verify(backend, bytes) catch return false;
    return true;
}

/// True when the gate refuses `bytes` with its last payload byte flipped.
/// The byte is restored before returning, so an admitted image stays
/// admitted (RA8FW-831).
pub fn refusesTamper(bytes: []u8) bool {
    if (bytes.len <= header_bytes) return false;
    const last = bytes.len - 1;
    bytes[last] ^= 0x01;
    defer bytes[last] ^= 0x01;
    return !admitted(bytes);
}
