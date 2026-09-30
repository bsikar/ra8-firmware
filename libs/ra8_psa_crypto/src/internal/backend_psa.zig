//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The on-target backend: a thin translation between `ra8_err_t` and
//! `psa_status_t` over the vendored TF-PSA-Crypto. Every PSA call the facade
//! makes is here, and the only interesting logic, the tag mapping, lives in
//! `psa_map.zig` where the host suite can reach it.

const map = @import("psa_map.zig");
const pool = @import("pool.zig");
const psa = @import("psa_c.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Limits = vocab.Limits;
const Slot = pool.Slot;
const c = psa.c;

fn translate(status: psa.status_t) u16 {
    return if (status == psa.success) Err.ok else Err.hw_error;
}

pub fn init() u16 {
    return translate(c.psa_crypto_init());
}

pub fn deinit() void {
    c.mbedtls_psa_crypto_free();
}

pub fn importKey(slot: *Slot, attr: *const vocab.KeyAttr, data: []const u8) u16 {
    var attributes = c.psa_key_attributes_init();
    c.psa_set_key_type(&attributes, map.keyType(psa, attr.type));
    c.psa_set_key_algorithm(&attributes, map.algorithm(psa, attr.alg));
    c.psa_set_key_usage_flags(&attributes, map.usage(psa, attr.usage));

    var key_id: psa.key_id_t = 0;
    const status = c.psa_import_key(&attributes, data.ptr, data.len, &key_id);
    if (status != psa.success) return Err.hw_error;
    slot.psa_id = key_id;
    return Err.ok;
}

pub fn destroyKey(slot: *Slot) void {
    _ = c.psa_destroy_key(slot.psa_id);
}

pub fn hash(input: []const u8, out: []u8) u16 {
    var produced: usize = 0;
    const status = c.psa_hash_compute(
        psa.alg_sha_256,
        input.ptr,
        input.len,
        out.ptr,
        out.len,
        &produced,
    );
    return translate(status);
}

pub fn signHash(slot: *const Slot, digest: []const u8, signature: []u8, out_len: *usize) u16 {
    var produced: usize = 0;
    const status = c.psa_sign_hash(
        slot.psa_id,
        psa.alg_ecdsa_sha_256,
        digest.ptr,
        digest.len,
        signature.ptr,
        signature.len,
        &produced,
    );
    if (status != psa.success) return Err.hw_error;
    out_len.* = produced;
    return Err.ok;
}

pub fn verifyHash(slot: *const Slot, digest: []const u8, signature: []const u8) u16 {
    const status = c.psa_verify_hash(
        slot.psa_id,
        psa.alg_ecdsa_sha_256,
        digest.ptr,
        digest.len,
        signature.ptr,
        signature.len,
    );
    if (status == psa.success) return Err.ok;
    if (status == psa.error_invalid_signature) return Err.crc_mismatch;
    return Err.hw_error;
}

pub fn aeadEncrypt(
    slot: *const Slot,
    nonce: []const u8,
    aad: []const u8,
    plain: []const u8,
    out: []u8,
    out_len: *usize,
) u16 {
    var produced: usize = 0;
    const status = c.psa_aead_encrypt(
        slot.psa_id,
        psa.alg_gcm,
        nonce.ptr,
        nonce.len,
        aad.ptr,
        aad.len,
        plain.ptr,
        plain.len,
        out.ptr,
        out.len,
        &produced,
    );
    if (status != psa.success) return Err.hw_error;
    out_len.* = produced;
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
    _ = plain_len;
    var produced: usize = 0;
    const status = c.psa_aead_decrypt(
        slot.psa_id,
        psa.alg_gcm,
        nonce.ptr,
        nonce.len,
        aad.ptr,
        aad.len,
        cipher.ptr,
        cipher.len,
        out.ptr,
        out.len,
        &produced,
    );
    if (status == psa.error_invalid_signature) return Err.crc_mismatch;
    if (status != psa.success) return Err.hw_error;
    out_len.* = produced;
    return Err.ok;
}

pub fn random(out: []u8) u16 {
    return translate(c.psa_generate_random(out.ptr, out.len));
}
