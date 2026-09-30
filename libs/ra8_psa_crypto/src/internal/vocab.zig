//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The words the facade speaks: error codes, static-pool limits, and the
//! project-local key-type / algorithm / usage tags that `ra8_psa_crypto.h`
//! publishes. Nothing here knows what PSA is.

/// `ra8_err_t` values this library can return (libs/ra8_core/inc/ra8_err.h).
pub const Err = struct {
    pub const ok: u16 = 0;
    pub const no_mem: u16 = 0x102;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_size: u16 = 0x105;
    pub const not_supported: u16 = 0x107;
    pub const exists: u16 = 0x10C;
    pub const not_initialized: u16 = 0x10F;
    pub const hw_error: u16 = 0x204;
    pub const crc_mismatch: u16 = 0x405;
};

/// `ra8_psa_limits_t`. NASA Power of 10 Rule 3: every bound is static.
pub const Limits = struct {
    pub const max_keys: usize = 16;
    pub const max_key_bytes: usize = 96;
    pub const sha256_len: usize = 32;
    pub const gcm_nonce_len: usize = 12;
    pub const gcm_tag_len: usize = 16;
    pub const max_sig_bytes: usize = 96;
};

/// `ra8_psa_key_type_t`. Open so a value off the wire round-trips as the C
/// `default:` arm did rather than triggering illegal-value behaviour.
pub const KeyType = enum(u8) {
    raw = 0,
    aes = 1,
    hmac = 2,
    ecc_p256_priv = 3,
    ecc_p256_pub = 4,
    _,
};

/// `ra8_psa_alg_t`.
pub const Alg = enum(u8) {
    none = 0,
    sha_256 = 1,
    aes_gcm = 2,
    ecdsa_sha_256 = 3,
    _,
};

/// `ra8_psa_key_usage_t` bits.
pub const Usage = struct {
    pub const none: u32 = 0x00;
    pub const sign: u32 = 0x01;
    pub const verify: u32 = 0x02;
    pub const encrypt: u32 = 0x04;
    pub const decrypt: u32 = 0x08;
    pub const derive: u32 = 0x10;

    pub fn has(usage: u32, bit: u32) bool {
        return (usage & bit) != 0;
    }
};

/// `ra8_psa_key_attr_t`, laid out as the C header declares it.
pub const KeyAttr = extern struct {
    type: KeyType,
    alg: Alg,
    usage: u32 align(4),
};
