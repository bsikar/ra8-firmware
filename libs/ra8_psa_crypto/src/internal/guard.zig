//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argument and state checks every public entry point runs before it reaches
//! a backend. Kept apart from `facade.zig` so the rejection contract can be
//! read, and tested, without a pool or a crypto implementation in the way.
//!
//! Each check is a separate single-condition branch: the C wrote several of
//! these as compound `||` decisions and the suite has MC/DC cases pinned to
//! them, so splitting keeps every condition individually observable.

const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Limits = vocab.Limits;

/// Re-exported because every signature below names them.
pub const Alg = vocab.Alg;
pub const KeyAttr = vocab.KeyAttr;
pub const Usage = vocab.Usage;

/// Shape of a slot as the guards need to see it: just the cached attributes.
pub const SlotView = struct {
    usage: u32,
};

/// `ra8_psa_key_import` argument quad plus the static caps.
pub fn keyImport(
    initialized: bool,
    attr: ?*const vocab.KeyAttr,
    data_len: usize,
    data_present: bool,
) u16 {
    if (attr == null) return Err.invalid_arg;
    if (!data_present) return Err.invalid_arg;
    if (data_len == 0) return Err.invalid_arg;
    if (!initialized) return Err.not_initialized;
    if (data_len > Limits.max_key_bytes) return Err.invalid_size;
    if (attr.?.usage == vocab.Usage.none) return Err.invalid_arg;
    return Err.ok;
}

/// `ra8_psa_hash_compute`.
pub fn hashCompute(
    initialized: bool,
    alg: vocab.Alg,
    input_present: bool,
    input_len: usize,
    out_cap: usize,
) u16 {
    if (!input_present and input_len != 0) return Err.invalid_arg;
    if (alg != .sha_256) return Err.invalid_arg;
    if (!initialized) return Err.not_initialized;
    if (out_cap < Limits.sha256_len) return Err.invalid_size;
    return Err.ok;
}

/// Shared by `ra8_psa_sign_hash` and `ra8_psa_verify_hash`: both demand an
/// ECDSA algorithm, a live handle carrying the right usage bit, and a
/// SHA-256-sized digest.
pub fn hashOperation(
    initialized: bool,
    slot: ?SlotView,
    alg: vocab.Alg,
    hash_len: usize,
    usage_bit: u32,
) u16 {
    if (!initialized) return Err.not_initialized;
    if (slot == null) return Err.invalid_arg;
    if (alg != .ecdsa_sha_256) return Err.not_supported;
    if (!vocab.Usage.has(slot.?.usage, usage_bit)) return Err.invalid_arg;
    if (hash_len != Limits.sha256_len) return Err.invalid_size;
    return Err.ok;
}

/// `ra8_psa_aead_encrypt`, after the null checks the membrane makes.
pub fn aeadEncrypt(
    initialized: bool,
    slot: ?SlotView,
    alg: vocab.Alg,
    nonce_len: usize,
    aad_present: bool,
    aad_len: usize,
    plain_present: bool,
    plain_len: usize,
    out_cap: usize,
) u16 {
    if (!plain_present and plain_len != 0) return Err.invalid_arg;
    if (!aad_present and aad_len != 0) return Err.invalid_arg;
    if (!initialized) return Err.not_initialized;
    if (slot == null) return Err.invalid_arg;
    if (alg != .aes_gcm) return Err.invalid_arg;
    if (!vocab.Usage.has(slot.?.usage, vocab.Usage.encrypt)) return Err.invalid_arg;
    if (nonce_len != Limits.gcm_nonce_len) return Err.invalid_size;
    if (out_cap < plain_len + Limits.gcm_tag_len) return Err.invalid_size;
    return Err.ok;
}

/// `ra8_psa_aead_decrypt`. On success the recovered plaintext length is the
/// ciphertext minus its trailing tag, which the caller needs, so it comes
/// back rather than being recomputed.
pub const DecryptPlan = union(enum) {
    reject: u16,
    accept: usize,
};

pub fn aeadDecrypt(
    initialized: bool,
    slot: ?SlotView,
    alg: vocab.Alg,
    nonce_len: usize,
    aad_present: bool,
    aad_len: usize,
    cipher_len: usize,
    out_present: bool,
    out_cap: usize,
) DecryptPlan {
    if (!aad_present and aad_len != 0) return .{ .reject = Err.invalid_arg };
    if (!initialized) return .{ .reject = Err.not_initialized };
    if (slot == null) return .{ .reject = Err.invalid_arg };
    if (alg != .aes_gcm) return .{ .reject = Err.invalid_arg };
    if (!vocab.Usage.has(slot.?.usage, vocab.Usage.decrypt)) return .{ .reject = Err.invalid_arg };
    if (nonce_len != Limits.gcm_nonce_len) return .{ .reject = Err.invalid_size };
    if (cipher_len < Limits.gcm_tag_len) return .{ .reject = Err.invalid_size };

    const plain_len = cipher_len - Limits.gcm_tag_len;
    if (!out_present and plain_len != 0) return .{ .reject = Err.invalid_arg };
    if (out_cap < plain_len) return .{ .reject = Err.invalid_size };
    return .{ .accept = plain_len };
}

/// `ra8_psa_crypto_random`.
pub fn random(initialized: bool, out_len: usize) u16 {
    if (out_len == 0) return Err.invalid_size;
    if (!initialized) return Err.not_initialized;
    return Err.ok;
}
