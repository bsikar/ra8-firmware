//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Admission gate half of txm_touch_led_m85 (RA8FW-1262): verifies the
//! signed `.ra8app` against the pinned Ed25519 test key under `display | led`,
//! proves the gate refuses withholding `led`, and proves tamper refusal
//! before loading.

const std = @import("std");
const appimg_verify = @import("app_resident").verify;

const Ed25519 = std.crypto.sign.Ed25519;

/// Ed25519 public key of test_key.zig's seed (0x24 repeated 32 times).
pub const public_key = [appimg_verify.pubkey_bytes]u8{
    0x58, 0x93, 0x66, 0x04, 0xab, 0xda, 0x11, 0x2b,
    0xc9, 0x49, 0x33, 0x56, 0x9c, 0x82, 0xf8, 0xd0,
    0xcc, 0x0d, 0xdf, 0x92, 0xa3, 0xf8, 0x32, 0x9f,
    0x2f, 0x44, 0x8f, 0x7f, 0x48, 0x4a, 0x59, 0x4c,
};

pub const cap_display: u32 = 0x1;
pub const cap_led: u32 = 0x8;
/// Host grant for this demo: display + LED access only (no storage/network).
pub const granted: u32 = cap_display | cap_led;
pub const header_bytes: usize = 160;

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

const full_backend = appimg_verify.Backend{
    .verify = &ed25519,
    .public_key = &public_key,
    .granted = granted,
};

const display_only_backend = appimg_verify.Backend{
    .verify = &ed25519,
    .public_key = &public_key,
    .granted = cap_display,
};

/// True when `bytes` is an `.ra8app` signed by the pinned key whose
/// capabilities fit `granted` (`display | led`).
pub fn admitted(bytes: []const u8) bool {
    _ = appimg_verify.verify(full_backend, bytes) catch return false;
    return true;
}

/// True when the admission gate refuses `bytes` if `led` is withheld.
pub fn refusesWithoutLed(bytes: []const u8) bool {
    _ = appimg_verify.verify(display_only_backend, bytes) catch |err| {
        return err == appimg_verify.Error.AccessDenied;
    };
    return false;
}

/// True when the gate refuses `bytes` with its last payload byte flipped.
pub fn refusesTamper(bytes: []u8) bool {
    if (bytes.len <= header_bytes) return false;
    const last = bytes.len - 1;
    bytes[last] ^= 0x01;
    defer bytes[last] ^= 0x01;
    return !admitted(bytes);
}

pub const Manifest = struct {
    app_id: []const u8,
    capabilities: u32,
};

pub fn manifest(bytes: []const u8) ?Manifest {
    if (bytes.len < header_bytes) return null;
    const capabilities = std.mem.readInt(u32, bytes[7 * @sizeOf(u32) ..][0..4], .little);
    const field = bytes[8 * @sizeOf(u32) ..][0..32];
    const end = std.mem.indexOfScalar(u8, field, 0) orelse field.len;
    if (end == 0) return null;
    return .{ .app_id = field[0..end], .capabilities = capabilities };
}
