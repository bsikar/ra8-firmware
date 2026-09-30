//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! End-to-end cover for the facade over the off-target backend: lifecycle,
//! pool exhaustion, and the crypto round-trips the C suite drives through
//! the public ABI.

const std = @import("std");
const facade = @import("facade");

const ok: u16 = 0;
const no_mem: u16 = 0x102;
const invalid_arg: u16 = 0x103;
const invalid_size: u16 = 0x105;
const exists: u16 = 0x10C;
const not_initialized: u16 = 0x10F;
const crc_mismatch: u16 = 0x405;

fn attrFor(usage: u32) @FieldType(facade.Slot, "attr") {
    return .{ .type = .aes, .alg = .aes_gcm, .usage = usage };
}

fn freshInit() !void {
    facade.resetForTest();
    try std.testing.expectEqual(ok, facade.init());
}

fn importKey(usage: u32, material: []const u8) !*facade.Slot {
    const a = attrFor(usage);
    var slot: *facade.Slot = undefined;
    try std.testing.expectEqual(ok, facade.keyImport(&a, material, &slot));
    return slot;
}

test "init succeeds once and reports exists on a second call" {
    try freshInit();
    try std.testing.expectEqual(exists, facade.init());
}

test "deinit before init is not initialized" {
    facade.resetForTest();
    try std.testing.expectEqual(not_initialized, facade.deinit());
}

test "init after deinit succeeds again" {
    try freshInit();
    try std.testing.expectEqual(ok, facade.deinit());
    try std.testing.expectEqual(ok, facade.init());
}

test "every entry point refuses an uninitialised module" {
    facade.resetForTest();
    var written: usize = 0;
    var out: [64]u8 = undefined;
    const a = attrFor(0x04);
    var slot: *facade.Slot = undefined;
    try std.testing.expectEqual(not_initialized, facade.keyImport(&a, "key", &slot));
    try std.testing.expectEqual(not_initialized, facade.keyDestroy(null));
    try std.testing.expectEqual(not_initialized, facade.hashCompute(.sha_256, "x", true, &out, &written));
    try std.testing.expectEqual(not_initialized, facade.random(out[0..8]));
}

test "an imported key comes back as a live handle" {
    try freshInit();
    const slot = try importKey(0x04, "0123456789abcdef");
    try std.testing.expect(slot.in_use);
    try std.testing.expectEqualSlices(u8, "0123456789abcdef", slot.material());
}

test "the pool runs out after sixteen keys" {
    try freshInit();
    for (0..16) |_| _ = try importKey(0x04, "k");
    const a = attrFor(0x04);
    var slot: *facade.Slot = undefined;
    try std.testing.expectEqual(no_mem, facade.keyImport(&a, "k", &slot));
}

test "destroying a key returns its slot to the pool" {
    try freshInit();
    var held: *facade.Slot = undefined;
    for (0..16) |i| {
        const slot = try importKey(0x04, "k");
        if (i == 0) held = slot;
    }
    const a = attrFor(0x04);
    var overflow: *facade.Slot = undefined;
    try std.testing.expectEqual(no_mem, facade.keyImport(&a, "k", &overflow));

    try std.testing.expectEqual(ok, facade.keyDestroy(held));
    try std.testing.expectEqual(ok, facade.keyImport(&a, "k", &overflow));
    try std.testing.expectEqual(held, overflow);
}

test "destroy rejects null and a slot-shaped address outside the pool" {
    try freshInit();
    try std.testing.expectEqual(invalid_arg, facade.keyDestroy(null));
    // A well-formed slot that simply is not one of ours, which is what the
    // security suite hands in when it casts a stack address to the handle.
    var stray: facade.Slot = std.mem.zeroes(facade.Slot);
    stray.in_use = true;
    try std.testing.expectEqual(invalid_arg, facade.keyDestroy(&stray));
}

test "destroy twice is rejected the second time" {
    try freshInit();
    const slot = try importKey(0x04, "key");
    try std.testing.expectEqual(ok, facade.keyDestroy(slot));
    try std.testing.expectEqual(invalid_arg, facade.keyDestroy(slot));
}

test "deinit releases every live key" {
    try freshInit();
    const slot = try importKey(0x04, "key");
    try std.testing.expectEqual(ok, facade.deinit());
    try std.testing.expect(!slot.in_use);
}

test "hash writes a 32 byte digest" {
    try freshInit();
    var out: [32]u8 = undefined;
    var written: usize = 0;
    try std.testing.expectEqual(ok, facade.hashCompute(.sha_256, "abc", true, &out, &written));
    try std.testing.expectEqual(@as(usize, 32), written);

    var reference: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash("abc", &reference, .{});
    try std.testing.expectEqualSlices(u8, &reference, &out);
}

test "sign then verify round-trips" {
    try freshInit();
    const signer = try importKey(0x01 | 0x02, "signing-key");
    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash("message", &digest, .{});

    var signature: [96]u8 = undefined;
    var written: usize = 0;
    try std.testing.expectEqual(
        ok,
        facade.signHash(signer, .ecdsa_sha_256, &digest, &signature, &written),
    );
    try std.testing.expectEqual(@as(usize, 32), written);
    try std.testing.expectEqual(
        ok,
        facade.verifyHash(signer, .ecdsa_sha_256, &digest, signature[0..written]),
    );
}

test "verify rejects a flipped signature bit and a wrong digest" {
    try freshInit();
    const signer = try importKey(0x01 | 0x02, "signing-key");
    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash("message", &digest, .{});
    var signature: [96]u8 = undefined;
    var written: usize = 0;
    _ = facade.signHash(signer, .ecdsa_sha_256, &digest, &signature, &written);

    signature[0] ^= 0x01;
    try std.testing.expectEqual(
        crc_mismatch,
        facade.verifyHash(signer, .ecdsa_sha_256, &digest, signature[0..written]),
    );
    signature[0] ^= 0x01;

    var other: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash("другое", &other, .{});
    try std.testing.expectEqual(
        crc_mismatch,
        facade.verifyHash(signer, .ecdsa_sha_256, &other, signature[0..written]),
    );
}

test "aead round-trips through the facade" {
    try freshInit();
    const key = try importKey(0x04 | 0x08, "aead-key");
    const nonce = "0123456789ab";
    const plain = "attack at dawn";

    var sealed: [plain.len + 16]u8 = undefined;
    var written: usize = 0;
    try std.testing.expectEqual(
        ok,
        facade.aeadEncrypt(key, .aes_gcm, nonce, "aad", true, plain, true, &sealed, &written),
    );
    try std.testing.expectEqual(@as(usize, plain.len + 16), written);

    var opened: [plain.len]u8 = undefined;
    var recovered: usize = 0;
    try std.testing.expectEqual(
        ok,
        facade.aeadDecrypt(key, .aes_gcm, nonce, "aad", true, &sealed, &opened, true, &recovered),
    );
    try std.testing.expectEqual(@as(usize, plain.len), recovered);
    try std.testing.expectEqualSlices(u8, plain, &opened);
}

test "aead decrypt rejects a tampered ciphertext" {
    try freshInit();
    const key = try importKey(0x04 | 0x08, "aead-key");
    const nonce = "0123456789ab";
    const plain = "attack at dawn";
    var sealed: [plain.len + 16]u8 = undefined;
    var written: usize = 0;
    _ = facade.aeadEncrypt(key, .aes_gcm, nonce, "aad", true, plain, true, &sealed, &written);
    sealed[2] ^= 0xFF;

    var opened: [plain.len]u8 = undefined;
    var recovered: usize = 0;
    try std.testing.expectEqual(
        crc_mismatch,
        facade.aeadDecrypt(key, .aes_gcm, nonce, "aad", true, &sealed, &opened, true, &recovered),
    );
}

test "aead enforces the usage bits" {
    try freshInit();
    const sealer = try importKey(0x04, "k");
    const nonce = "0123456789ab";
    var sealed: [16]u8 = undefined;
    var written: usize = 0;
    try std.testing.expectEqual(
        ok,
        facade.aeadEncrypt(sealer, .aes_gcm, nonce, "", true, "", true, &sealed, &written),
    );
    var opened: [0]u8 = undefined;
    var recovered: usize = 0;
    try std.testing.expectEqual(
        invalid_arg,
        facade.aeadDecrypt(sealer, .aes_gcm, nonce, "", true, &sealed, &opened, true, &recovered),
    );
}

test "random fills the buffer and advances" {
    try freshInit();
    var a: [32]u8 = undefined;
    var b: [32]u8 = undefined;
    try std.testing.expectEqual(ok, facade.random(&a));
    try std.testing.expectEqual(ok, facade.random(&b));
    try std.testing.expect(!std.mem.eql(u8, &a, &b));
}

test "random refuses a zero-length request" {
    try freshInit();
    var out: [1]u8 = undefined;
    try std.testing.expectEqual(invalid_size, facade.random(out[0..0]));
}
