//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The signature half of txm_sd_hello_m85 (RA8FW-830): admit an .ra8app
//! through ra8_app's admission gate (appimg_verify.verify: container, grant,
//! then signature) with the production Ed25519 backend (RA8FW-291): the same
//! `ra8_appimg_ed25519_verify` export silicon will run, built into this
//! example instead of an inline copy of the crypto. The pinned key is the one
//! the `.ra8_nsc_appimg_pubkey` Secure-flash section carries
//! (ra8_nsc_appimg_pubkey, RA8FW-291), whose shipped default derives from
//! libs/ra8_app/tools/test_key.zig's seed, so only images
//! `zig build txm-hello-m33` signs pass.

const appimg_verify = @import("appimg_verify");
const production = @import("ed25519_backend");

const pinned_pubkey = @import("ra8_nsc_appimg_pubkey");

/// The pinned Ed25519 public key: the `.ra8_nsc_appimg_pubkey` Secure-flash
/// section, read secure-side (RA8FW-291). Provisioning replaces the key by
/// rewriting that section.
pub const public_key = pinned_pubkey.pinned();
/// Capability bits this host grants: none; txm_hello_m33 asks for none.
pub const granted: u32 = 0;
/// sizeof(appimg.Header): the module payload starts here.
pub const header_bytes = 160;

/// Admission runs the production backend: the WireMsg mirror of
/// ra8_appimg_msg_t carries the signed run to `ra8_appimg_ed25519_verify`,
/// and only its `ok` admits. `not_supported` passes through as the one
/// refusal the gate reports verbatim; every other code is a failed verify.
fn ed25519(
    ctx: ?*anyopaque,
    msg: appimg_verify.Message,
    signature: *const [64]u8, // Ed25519 raw R||S, appimg.Width.signature
    key: *const [appimg_verify.pubkey_bytes]u8,
) appimg_verify.Verdict {
    _ = ctx;
    const wire = production.WireMsg{
        .head = msg.head.ptr,
        .tail = msg.tail.ptr,
        .head_len = @intCast(msg.head.len),
        .tail_len = @intCast(msg.tail.len),
    };
    return switch (production.ra8_appimg_ed25519_verify(null, &wire, signature, key)) {
        @backingInt(production.Code.ok) => .good,
        @backingInt(production.Code.not_supported) => .unsupported,
        else => .bad,
    };
}

const backend = appimg_verify.Backend{ .verify = &ed25519, .public_key = public_key, .granted = granted };

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
