//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Translation of the project-local tags in `vocab.zig` into the PSA Crypto
//! spelling. Generic over the constant provider so the host suite can check
//! the switch arms against sentinels without the vendored headers on its
//! include path, exactly as the C's `internal_map_*` helpers were unreachable
//! from an `RA8_OFF_TARGET` build.

const vocab = @import("vocab.zig");

/// `ra8_psa_key_type_t` -> `PSA_KEY_TYPE_*`. Anything unrecognised lands on
/// raw data, which is the C's `default:` arm.
pub fn keyType(comptime Psa: type, key_type: vocab.KeyType) @TypeOf(Psa.key_type_raw_data) {
    return switch (key_type) {
        .aes => Psa.key_type_aes,
        .hmac => Psa.key_type_hmac,
        .ecc_p256_priv => Psa.key_type_ecc_p256_key_pair,
        .ecc_p256_pub => Psa.key_type_ecc_p256_public_key,
        .raw => Psa.key_type_raw_data,
        _ => Psa.key_type_raw_data,
    };
}

/// `ra8_psa_alg_t` -> `PSA_ALG_*`. `none` and anything unrecognised map to 0,
/// which PSA reads as "no algorithm".
pub fn algorithm(comptime Psa: type, alg: vocab.Alg) @TypeOf(Psa.alg_sha_256) {
    return switch (alg) {
        .aes_gcm => Psa.alg_gcm,
        .ecdsa_sha_256 => Psa.alg_ecdsa_sha_256,
        .sha_256 => Psa.alg_sha_256,
        .none => 0,
        _ => 0,
    };
}

/// `ra8_psa_key_usage_t` bitmask -> `PSA_KEY_USAGE_*` flags.
pub fn usage(comptime Psa: type, bits: u32) @TypeOf(Psa.usage_sign_hash) {
    var out: @TypeOf(Psa.usage_sign_hash) = 0;
    if (vocab.Usage.has(bits, vocab.Usage.sign)) out |= Psa.usage_sign_hash;
    if (vocab.Usage.has(bits, vocab.Usage.verify)) out |= Psa.usage_verify_hash;
    if (vocab.Usage.has(bits, vocab.Usage.encrypt)) out |= Psa.usage_encrypt;
    if (vocab.Usage.has(bits, vocab.Usage.decrypt)) out |= Psa.usage_decrypt;
    if (vocab.Usage.has(bits, vocab.Usage.derive)) out |= Psa.usage_derive;
    return out;
}
