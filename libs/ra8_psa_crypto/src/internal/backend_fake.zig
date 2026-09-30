//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `RA8_OFF_TARGET` backend. Key material lives in the pool slot and the
//! primitives are the stand-ins in `fake.zig`, so the host suite can drive
//! the whole facade without TF-PSA-Crypto linked.

const fake = @import("fake.zig");
const pool = @import("pool.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Limits = vocab.Limits;
const Slot = pool.Slot;

var rng: fake.Rng = .{};

pub fn init() u16 {
    return Err.ok;
}

pub fn deinit() void {}

pub fn importKey(slot: *Slot, attr: *const vocab.KeyAttr, data: []const u8) u16 {
    _ = slot;
    _ = attr;
    _ = data;
    return Err.ok;
}

pub fn destroyKey(slot: *Slot) void {
    _ = slot;
}

pub fn hash(input: []const u8, out: []u8) u16 {
    var digest: [Limits.sha256_len]u8 = undefined;
    fake.sha256(input, &digest);
    @memcpy(out[0..Limits.sha256_len], &digest);
    return Err.ok;
}

/// Stand-in "signature": SHA-256(key || hash), 32 bytes.
fn bind(slot: *const Slot, digest: []const u8, out: *[Limits.sha256_len]u8) void {
    var buffer: [Limits.max_key_bytes + Limits.sha256_len]u8 = undefined;
    const key = slot.material();
    @memcpy(buffer[0..key.len], key);
    @memcpy(buffer[key.len..][0..digest.len], digest);
    fake.sha256(buffer[0 .. key.len + digest.len], out);
}

pub fn signHash(slot: *const Slot, digest: []const u8, signature: []u8, out_len: *usize) u16 {
    if (signature.len < Limits.sha256_len) return Err.invalid_size;
    var bound: [Limits.sha256_len]u8 = undefined;
    bind(slot, digest, &bound);
    @memcpy(signature[0..Limits.sha256_len], &bound);
    out_len.* = Limits.sha256_len;
    return Err.ok;
}

pub fn verifyHash(slot: *const Slot, digest: []const u8, signature: []const u8) u16 {
    if (signature.len != Limits.sha256_len) return Err.crc_mismatch;
    var bound: [Limits.sha256_len]u8 = undefined;
    bind(slot, digest, &bound);
    const std = @import("std");
    if (!std.crypto.utils.timingSafeEql(
        [Limits.sha256_len]u8,
        bound,
        signature[0..Limits.sha256_len].*,
    )) return Err.crc_mismatch;
    return Err.ok;
}

pub fn aeadEncrypt(
    slot: *const Slot,
    nonce: []const u8,
    aad: []const u8,
    plain: []const u8,
    out: []u8,
    out_len: *usize,
) u16 {
    const status = fake.aeadEncrypt(slot.material(), nonce, aad, plain, out);
    if (status != Err.ok) return status;
    out_len.* = plain.len + Limits.gcm_tag_len;
    return Err.ok;
}

pub fn aeadDecrypt(
    slot: *const Slot,
    nonce: []const u8,
    aad: []const u8,
    cipher: []const u8,
    out: []u8,
    plain_len: usize,
    out_len: *usize,
) u16 {
    const status = fake.aeadDecrypt(slot.material(), nonce, aad, cipher, out);
    if (status != Err.ok) return status;
    out_len.* = plain_len;
    return Err.ok;
}

pub fn random(out: []u8) u16 {
    rng.fill(out);
    return Err.ok;
}
