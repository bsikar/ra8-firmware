//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one place that reads the vendored TF-PSA-Crypto headers. Everything
//! else in the library sees the constant names below, never a `PSA_*` macro,
//! so `psa_map.zig` stays testable against a stub provider.
//!
//! `build.zig` translates `psa/crypto.h` into the `psa_h` module with the
//! include roots and the two config-file defines the CMake build already
//! passes to the C (`cmake/mbedtls.cmake`).

pub const c = @import("psa_h");

pub const status_t = c.psa_status_t;
pub const key_id_t = c.psa_key_id_t;

pub const success = c.PSA_SUCCESS;
pub const error_invalid_signature = c.PSA_ERROR_INVALID_SIGNATURE;

pub const key_type_raw_data = c.PSA_KEY_TYPE_RAW_DATA;
pub const key_type_aes = c.PSA_KEY_TYPE_AES;
pub const key_type_hmac = c.PSA_KEY_TYPE_HMAC;
pub const key_type_ecc_p256_key_pair = c.PSA_KEY_TYPE_ECC_KEY_PAIR(c.PSA_ECC_FAMILY_SECP_R1);
pub const key_type_ecc_p256_public_key = c.PSA_KEY_TYPE_ECC_PUBLIC_KEY(c.PSA_ECC_FAMILY_SECP_R1);

pub const alg_sha_256 = c.PSA_ALG_SHA_256;
pub const alg_gcm = c.PSA_ALG_GCM;
pub const alg_ecdsa_sha_256 = c.PSA_ALG_ECDSA(c.PSA_ALG_SHA_256);

pub const usage_sign_hash = c.PSA_KEY_USAGE_SIGN_HASH;
pub const usage_verify_hash = c.PSA_KEY_USAGE_VERIFY_HASH;
pub const usage_encrypt = c.PSA_KEY_USAGE_ENCRYPT;
pub const usage_decrypt = c.PSA_KEY_USAGE_DECRYPT;
pub const usage_derive = c.PSA_KEY_USAGE_DERIVE;
