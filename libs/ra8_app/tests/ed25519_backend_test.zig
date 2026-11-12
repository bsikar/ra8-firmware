//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The production Ed25519 backend end to end (RA8FW-291): an image the
//! packer signs with the bench seed is admitted through the gate with the
//! production backend behind it, and the tampered, unsigned and wrong-key
//! cases are refused, in the gate's shape and through the raw C ABI the
//! verifier's `verify` member carries.

const std = @import("std");
const Ed25519 = std.crypto.sign.Ed25519;
const pack = @import("appimg_pack");
const backend_lib = @import("ed25519_backend");

// The gate types come from the packer's tree, so the gate end to end here is
// the one the packer's tests drive; the backend joins it the way a C caller
// joins it, through the fn pointer in the verifier struct.
const gate = pack.gate;
const appimg = gate.image;
const header_bytes = @sizeOf(appimg.Header);
const signature_len = appimg.Width.signature;

const code = [_]u8{ 0x00, 0xBF, 0x70, 0x47, 0x01, 0x02, 0x03, 0x04 };
const data = [_]u8{ 0xAA, 0xBB, 0xCC, 0xDD };

const manifest: pack.Manifest = .{
    .entry_offset = 4,
    .stack_size = 0x800,
    .capabilities = appimg.Capability.display,
    .app_id = "com.ra8.verify",
    .display_name = "Verify",
};

fn keyPair(fill: u8) !Ed25519.KeyPair {
    return Ed25519.KeyPair.generateDeterministic(@as([Ed25519.KeyPair.seed_length]u8, @splat(fill)));
}

fn signedImage(key_pair: Ed25519.KeyPair) ![]u8 {
    return pack.pack(std.testing.allocator, manifest, &code, &data, key_pair);
}

/// The production export behind the gate's fn pointer, the wiring a C caller
/// builds (`ra8_appimg_verifier_t.verify` -> `ra8_appimg_ed25519_verify`).
fn productionBackend(public_key: *const [gate.pubkey_bytes]u8, granted: u32) gate.Backend {
    return .{ .verify = &cVerdict, .public_key = public_key, .granted = granted };
}

fn cVerdict(
    ctx: ?*anyopaque,
    msg: gate.Message,
    signature: *const [signature_len]u8,
    public_key: *const [gate.pubkey_bytes]u8,
) gate.Verdict {
    const runs = backend_lib.WireMsg{
        .head = msg.head.ptr,
        .tail = msg.tail.ptr,
        .head_len = @intCast(msg.head.len),
        .tail_len = @intCast(msg.tail.len),
    };
    const verdict = backend_lib.ra8_appimg_ed25519_verify(ctx, &runs, signature, public_key);
    return switch (verdict) {
        0 => .good,
        else => .bad,
    };
}

/// The wire view of an already-parsed image, ready for a direct C ABI call.
fn wire(image: []const u8) !struct { msg: backend_lib.WireMsg, sig: [*]const u8 } {
    const header = try appimg.parse(image);
    const msg = try gate.signedMessage(header, image);
    const signature = image[appimg.Header.signature_offset..][0..signature_len];
    return .{
        .msg = .{
            .head = msg.head.ptr,
            .tail = msg.tail.ptr,
            .head_len = @intCast(msg.head.len),
            .tail_len = @intCast(msg.tail.len),
        },
        .sig = signature.ptr,
    };
}

test "a packed image is admitted with the production backend, gate and C ABI alike" {
    const key_pair = try keyPair(0x24); // the bench seed
    const bytes = try signedImage(key_pair);
    defer std.testing.allocator.free(bytes);

    const public_key = key_pair.public_key.toBytes();
    const header = try gate.verify(productionBackend(&public_key, appimg.Capability.known), bytes);
    try std.testing.expectEqualStrings("com.ra8.verify", std.mem.sliceTo(&header.app_id, 0));

    const w = try wire(bytes);
    const c_verdict = backend_lib.ra8_appimg_ed25519_verify(null, &w.msg, w.sig, &public_key);
    try std.testing.expectEqual(@as(u16, 0), c_verdict);
}

test "a flipped payload byte is refused as a failed verify" {
    const key_pair = try keyPair(0x24);
    const bytes = try signedImage(key_pair);
    defer std.testing.allocator.free(bytes);

    const last = bytes.len - 1;
    bytes[last] ^= 0x01;
    defer bytes[last] ^= 0x01;

    const public_key = key_pair.public_key.toBytes();
    try std.testing.expectError(
        error.BadSignature,
        gate.verify(productionBackend(&public_key, appimg.Capability.known), bytes),
    );

    const w = try wire(bytes);
    const c_verdict = backend_lib.ra8_appimg_ed25519_verify(null, &w.msg, w.sig, &public_key);
    try std.testing.expectEqual(@as(u16, 0x501), c_verdict);
}

test "an unsigned image is refused by the backend" {
    const key_pair = try keyPair(0x24);
    const bytes = try signedImage(key_pair);
    defer std.testing.allocator.free(bytes);

    @memset(bytes[appimg.Header.signature_offset..][0..signature_len], 0);
    const public_key = key_pair.public_key.toBytes();

    const w = try wire(bytes);
    const c_verdict = backend_lib.ra8_appimg_ed25519_verify(null, &w.msg, w.sig, &public_key);
    try std.testing.expectEqual(@as(u16, 0x501), c_verdict);
}

test "a different pinned key refuses the same image" {
    const signer = try keyPair(0x24);
    const bytes = try signedImage(signer);
    defer std.testing.allocator.free(bytes);

    const stranger = try keyPair(0x42);
    const wrong_key = stranger.public_key.toBytes();

    const w = try wire(bytes);
    const c_verdict = backend_lib.ra8_appimg_ed25519_verify(null, &w.msg, w.sig, &wrong_key);
    try std.testing.expectEqual(@as(u16, 0x501), c_verdict);
}

test "NULL arguments are refused with null_ptr" {
    const key_pair = try keyPair(0x24);
    const bytes = try signedImage(key_pair);
    defer std.testing.allocator.free(bytes);
    const public_key = key_pair.public_key.toBytes();
    const signature = bytes[appimg.Header.signature_offset..][0..signature_len];

    const c_verdict = backend_lib.ra8_appimg_ed25519_verify(null, null, signature.ptr, &public_key);
    try std.testing.expectEqual(@as(u16, 0x504), c_verdict);
}
