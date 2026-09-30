//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host cover for the rejection contract. The C wrote several of these as
//! compound `||` decisions with MC/DC cases pinned to them, so each
//! condition gets a case of its own here.

const std = @import("std");
const guard = @import("guard");

const ok: u16 = 0;
const invalid_arg: u16 = 0x103;
const invalid_size: u16 = 0x105;
const not_supported: u16 = 0x107;
const not_initialized: u16 = 0x10F;

const Usage = guard.Usage;

fn attr(usage: u32) guard.KeyAttr {
    return .{ .type = .aes, .alg = .aes_gcm, .usage = usage };
}

const signer: guard.SlotView = .{ .usage = Usage.sign };
const verifier: guard.SlotView = .{ .usage = Usage.verify };
const sealer: guard.SlotView = .{ .usage = Usage.encrypt };
const opener: guard.SlotView = .{ .usage = Usage.decrypt };

// -- key import -------------------------------------------------------------

test "key import accepts a well-formed request" {
    const a = attr(Usage.encrypt);
    try std.testing.expectEqual(ok, guard.keyImport(true, &a, 16, true));
}

test "key import rejects each argument of the C's quad on its own" {
    const a = attr(Usage.encrypt);
    try std.testing.expectEqual(invalid_arg, guard.keyImport(true, null, 16, true));
    try std.testing.expectEqual(invalid_arg, guard.keyImport(true, &a, 16, false));
    try std.testing.expectEqual(invalid_arg, guard.keyImport(true, &a, 0, true));
}

test "key import reports uninitialised before it measures the key" {
    const a = attr(Usage.encrypt);
    try std.testing.expectEqual(not_initialized, guard.keyImport(false, &a, 4096, true));
}

test "key import caps the key at 96 bytes" {
    const a = attr(Usage.encrypt);
    try std.testing.expectEqual(ok, guard.keyImport(true, &a, 96, true));
    try std.testing.expectEqual(invalid_size, guard.keyImport(true, &a, 97, true));
}

test "key import rejects a key nobody may use" {
    const a = attr(Usage.none);
    try std.testing.expectEqual(invalid_arg, guard.keyImport(true, &a, 16, true));
}

// -- hash -------------------------------------------------------------------

test "hash accepts sha-256 with room for the digest" {
    try std.testing.expectEqual(ok, guard.hashCompute(true, .sha_256, true, 10, 32));
}

test "hash accepts an empty input only when the pointer is absent too" {
    try std.testing.expectEqual(ok, guard.hashCompute(true, .sha_256, false, 0, 32));
    try std.testing.expectEqual(invalid_arg, guard.hashCompute(true, .sha_256, false, 1, 32));
}

test "hash refuses any algorithm but sha-256" {
    inline for (.{ .none, .aes_gcm, .ecdsa_sha_256 }) |alg| {
        try std.testing.expectEqual(invalid_arg, guard.hashCompute(true, alg, true, 1, 32));
    }
}

test "hash needs 32 bytes of output room" {
    try std.testing.expectEqual(invalid_size, guard.hashCompute(true, .sha_256, true, 1, 31));
}

test "hash reports uninitialised after the argument checks" {
    try std.testing.expectEqual(not_initialized, guard.hashCompute(false, .sha_256, true, 1, 32));
    try std.testing.expectEqual(invalid_arg, guard.hashCompute(false, .aes_gcm, true, 1, 32));
}

// -- sign / verify ----------------------------------------------------------

test "hash operation accepts a signer and a verifier" {
    try std.testing.expectEqual(ok, guard.hashOperation(true, signer, .ecdsa_sha_256, 32, Usage.sign));
    try std.testing.expectEqual(ok, guard.hashOperation(true, verifier, .ecdsa_sha_256, 32, Usage.verify));
}

test "hash operation rejects a dead handle" {
    try std.testing.expectEqual(invalid_arg, guard.hashOperation(true, null, .ecdsa_sha_256, 32, Usage.sign));
}

test "hash operation calls a non-ecdsa algorithm unsupported, not invalid" {
    try std.testing.expectEqual(
        not_supported,
        guard.hashOperation(true, signer, .aes_gcm, 32, Usage.sign),
    );
}

test "hash operation enforces the usage bit" {
    try std.testing.expectEqual(
        invalid_arg,
        guard.hashOperation(true, verifier, .ecdsa_sha_256, 32, Usage.sign),
    );
    try std.testing.expectEqual(
        invalid_arg,
        guard.hashOperation(true, signer, .ecdsa_sha_256, 32, Usage.verify),
    );
}

test "hash operation demands a 32 byte digest" {
    try std.testing.expectEqual(
        invalid_size,
        guard.hashOperation(true, signer, .ecdsa_sha_256, 31, Usage.sign),
    );
}

test "hash operation reports uninitialised first" {
    try std.testing.expectEqual(
        not_initialized,
        guard.hashOperation(false, null, .aes_gcm, 0, Usage.sign),
    );
}

// -- aead encrypt -----------------------------------------------------------

test "aead encrypt accepts a sealed request" {
    try std.testing.expectEqual(ok, guard.aeadEncrypt(true, sealer, .aes_gcm, 12, true, 3, true, 8, 24));
}

test "aead encrypt rejects a length without its buffer" {
    try std.testing.expectEqual(
        invalid_arg,
        guard.aeadEncrypt(true, sealer, .aes_gcm, 12, true, 3, false, 8, 24),
    );
    try std.testing.expectEqual(
        invalid_arg,
        guard.aeadEncrypt(true, sealer, .aes_gcm, 12, false, 3, true, 8, 24),
    );
}

test "aead encrypt allows an absent aad and plaintext at length zero" {
    try std.testing.expectEqual(ok, guard.aeadEncrypt(true, sealer, .aes_gcm, 12, false, 0, false, 0, 16));
}

test "aead encrypt needs the gcm algorithm and the encrypt bit" {
    try std.testing.expectEqual(
        invalid_arg,
        guard.aeadEncrypt(true, sealer, .ecdsa_sha_256, 12, true, 0, true, 0, 16),
    );
    try std.testing.expectEqual(
        invalid_arg,
        guard.aeadEncrypt(true, opener, .aes_gcm, 12, true, 0, true, 0, 16),
    );
}

test "aead encrypt pins the nonce at 12 bytes" {
    try std.testing.expectEqual(
        invalid_size,
        guard.aeadEncrypt(true, sealer, .aes_gcm, 11, true, 0, true, 0, 16),
    );
    try std.testing.expectEqual(
        invalid_size,
        guard.aeadEncrypt(true, sealer, .aes_gcm, 13, true, 0, true, 0, 16),
    );
}

test "aead encrypt needs room for the plaintext and the tag" {
    try std.testing.expectEqual(ok, guard.aeadEncrypt(true, sealer, .aes_gcm, 12, true, 0, true, 8, 24));
    try std.testing.expectEqual(
        invalid_size,
        guard.aeadEncrypt(true, sealer, .aes_gcm, 12, true, 0, true, 8, 23),
    );
}

// -- aead decrypt -----------------------------------------------------------

fn plainLen(plan: guard.DecryptPlan) !usize {
    return switch (plan) {
        .accept => |len| len,
        .reject => error.Rejected,
    };
}

fn rejection(plan: guard.DecryptPlan) !u16 {
    return switch (plan) {
        .reject => |status| status,
        .accept => error.Accepted,
    };
}

test "aead decrypt returns the plaintext length behind the tag" {
    const plan = guard.aeadDecrypt(true, opener, .aes_gcm, 12, true, 0, 24, true, 8);
    try std.testing.expectEqual(@as(usize, 8), try plainLen(plan));
}

test "aead decrypt accepts a tag-only ciphertext" {
    const plan = guard.aeadDecrypt(true, opener, .aes_gcm, 12, false, 0, 16, false, 0);
    try std.testing.expectEqual(@as(usize, 0), try plainLen(plan));
}

test "aead decrypt refuses a ciphertext too short to hold a tag" {
    const plan = guard.aeadDecrypt(true, opener, .aes_gcm, 12, true, 0, 15, true, 8);
    try std.testing.expectEqual(invalid_size, try rejection(plan));
}

test "aead decrypt refuses an output buffer smaller than the plaintext" {
    const plan = guard.aeadDecrypt(true, opener, .aes_gcm, 12, true, 0, 24, true, 7);
    try std.testing.expectEqual(invalid_size, try rejection(plan));
}

test "aead decrypt refuses a missing output buffer for a real plaintext" {
    const plan = guard.aeadDecrypt(true, opener, .aes_gcm, 12, true, 0, 24, false, 8);
    try std.testing.expectEqual(invalid_arg, try rejection(plan));
}

test "aead decrypt needs the gcm algorithm and the decrypt bit" {
    try std.testing.expectEqual(
        invalid_arg,
        try rejection(guard.aeadDecrypt(true, opener, .sha_256, 12, true, 0, 24, true, 8)),
    );
    try std.testing.expectEqual(
        invalid_arg,
        try rejection(guard.aeadDecrypt(true, sealer, .aes_gcm, 12, true, 0, 24, true, 8)),
    );
}

test "aead decrypt rejects a dead handle and an uninitialised module" {
    try std.testing.expectEqual(
        invalid_arg,
        try rejection(guard.aeadDecrypt(true, null, .aes_gcm, 12, true, 0, 24, true, 8)),
    );
    try std.testing.expectEqual(
        not_initialized,
        try rejection(guard.aeadDecrypt(false, opener, .aes_gcm, 12, true, 0, 24, true, 8)),
    );
}

// -- random -----------------------------------------------------------------

test "random wants at least one byte, and the module up" {
    try std.testing.expectEqual(ok, guard.random(true, 1));
    try std.testing.expectEqual(invalid_size, guard.random(true, 0));
    try std.testing.expectEqual(not_initialized, guard.random(false, 1));
    // Length is checked before state, as the C did.
    try std.testing.expectEqual(invalid_size, guard.random(false, 0));
}
