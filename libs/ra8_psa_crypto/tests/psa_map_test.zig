//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host cover for the PSA tag mapping. The real provider is the vendored
//! header, which an off-target build never has on its include path, so the
//! switch arms are checked against a stub whose constants are distinct
//! sentinels: what can go wrong here is an arm pointing at the wrong peer,
//! and sentinels catch exactly that.

const std = @import("std");
const map = @import("psa_map");

/// Stands in for `psa_c.zig`. Every value is distinct so a crossed arm fails.
const Stub = struct {
    pub const key_type_raw_data: u16 = 0x1001;
    pub const key_type_aes: u16 = 0x1002;
    pub const key_type_hmac: u16 = 0x1003;
    pub const key_type_ecc_p256_key_pair: u16 = 0x1004;
    pub const key_type_ecc_p256_public_key: u16 = 0x1005;

    pub const alg_sha_256: u32 = 0x2001;
    pub const alg_gcm: u32 = 0x2002;
    pub const alg_ecdsa_sha_256: u32 = 0x2003;

    pub const usage_sign_hash: u32 = 0x0001;
    pub const usage_verify_hash: u32 = 0x0002;
    pub const usage_encrypt: u32 = 0x0004;
    pub const usage_decrypt: u32 = 0x0008;
    pub const usage_derive: u32 = 0x0010;
};

const Usage = struct {
    const sign: u32 = 0x01;
    const verify: u32 = 0x02;
    const encrypt: u32 = 0x04;
    const decrypt: u32 = 0x08;
    const derive: u32 = 0x10;
};

test "every key type reaches its own psa peer" {
    try std.testing.expectEqual(Stub.key_type_aes, map.keyType(Stub, .aes));
    try std.testing.expectEqual(Stub.key_type_hmac, map.keyType(Stub, .hmac));
    try std.testing.expectEqual(Stub.key_type_ecc_p256_key_pair, map.keyType(Stub, .ecc_p256_priv));
    try std.testing.expectEqual(Stub.key_type_ecc_p256_public_key, map.keyType(Stub, .ecc_p256_pub));
    try std.testing.expectEqual(Stub.key_type_raw_data, map.keyType(Stub, .raw));
}

test "an unknown key type falls back to raw data" {
    try std.testing.expectEqual(Stub.key_type_raw_data, map.keyType(Stub, @enumFromInt(0x7F)));
}

test "every algorithm reaches its own psa peer" {
    try std.testing.expectEqual(Stub.alg_gcm, map.algorithm(Stub, .aes_gcm));
    try std.testing.expectEqual(Stub.alg_ecdsa_sha_256, map.algorithm(Stub, .ecdsa_sha_256));
    try std.testing.expectEqual(Stub.alg_sha_256, map.algorithm(Stub, .sha_256));
}

test "none and an unknown algorithm map to zero" {
    try std.testing.expectEqual(@as(u32, 0), map.algorithm(Stub, .none));
    try std.testing.expectEqual(@as(u32, 0), map.algorithm(Stub, @enumFromInt(0x7F)));
}

test "each usage bit lights its own psa flag" {
    try std.testing.expectEqual(Stub.usage_sign_hash, map.usage(Stub, Usage.sign));
    try std.testing.expectEqual(Stub.usage_verify_hash, map.usage(Stub, Usage.verify));
    try std.testing.expectEqual(Stub.usage_encrypt, map.usage(Stub, Usage.encrypt));
    try std.testing.expectEqual(Stub.usage_decrypt, map.usage(Stub, Usage.decrypt));
    try std.testing.expectEqual(Stub.usage_derive, map.usage(Stub, Usage.derive));
}

test "usage bits combine rather than overwrite" {
    const combined = map.usage(Stub, Usage.encrypt | Usage.decrypt);
    try std.testing.expectEqual(Stub.usage_encrypt | Stub.usage_decrypt, combined);
}

test "no usage bits is no psa flags" {
    try std.testing.expectEqual(@as(u32, 0), map.usage(Stub, 0));
}

test "an unmapped usage bit is dropped, not passed through" {
    try std.testing.expectEqual(@as(u32, 0), map.usage(Stub, 0x8000));
}
