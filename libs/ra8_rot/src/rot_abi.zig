//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C membrane for the root of trust: the three `ra8_rot_*` symbols
//! `inc/ra8_rot.h` declares, the provisioned root public key, and the only
//! place in this port that speaks to a crypto engine.
//!
//! The decision table is in `internal/rot.zig` and is driven by
//! `tests/rot_test.zig` with no engine at all. What is left here is the
//! sequencing the gate owes its callers: screen the trailer, re-compute the
//! body digest, compare it, bind the anti-rollback version in, and verify the
//! signature over THAT material. The signature is the authority; the
//! trailer's stored digest is only a fast pre-check.

const builtin = @import("builtin");
const rot = @import("rot");

/// `ra8_err_t` codes this membrane returns. `ra8_err_t` is a C23
/// `enum : uint16_t`, so these are the ABI values.
const Err = struct {
    pub const ok: u16 = 0;
    pub const invalid_size: u16 = 0x105;
    pub const exists: u16 = 0x10C;
    pub const crc_mismatch: u16 = 0x405;
    pub const validation_failed: u16 = 0x501;
    pub const checksum_mismatch: u16 = 0x502;
    pub const null_ptr: u16 = 0x504;
};

/// PSA vocabulary, as `ra8_psa_crypto.h` numbers it.
const Psa = struct {
    pub const alg_sha_256: u8 = 1;
    pub const alg_ecdsa_sha_256: u8 = 3;
    pub const key_type_ecc_p256_pub: u8 = 4;
    pub const usage_verify: u32 = 0x02;
};

/// Mirrors `ra8_psa_key_attr_t`: `{ uint8_t type, uint8_t alg, uint32_t usage }`.
const KeyAttr = extern struct {
    type: u8,
    alg: u8,
    usage: u32,
};

comptime {
    if (@sizeOf(KeyAttr) != 8) @compileError("ra8_psa_key_attr_t must be 8 bytes");
    if (@offsetOf(KeyAttr, "usage") != 4) @compileError("usage must sit at offset 4");
}

/// Off target the RSIP hash engine does not exist, so the `ra8_psa` software
/// SHA-256 stand-in computes the digest instead. Both produce the identical
/// 32-byte result, which is what lets the host suite drive the real gate.
const off_target = builtin.target.os.tag != .freestanding;

const tag: [*:0]const u8 = "ROT";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

extern fn ra8_psa_crypto_init() u16;
extern fn ra8_psa_key_import(
    out_handle: *?*anyopaque,
    attr: *const KeyAttr,
    data: [*]const u8,
    data_len: usize,
) u16;
extern fn ra8_psa_key_destroy(handle: ?*anyopaque) u16;
extern fn ra8_psa_hash_compute(
    alg: u8,
    input: [*]const u8,
    input_len: usize,
    out: [*]u8,
    out_cap: usize,
    out_len: *usize,
) u16;
extern fn ra8_psa_verify_hash(
    handle: ?*anyopaque,
    alg: u8,
    hash: [*]const u8,
    hash_len: usize,
    sig: [*]const u8,
    sig_len: usize,
) u16;

/// HUM Ch 52.2.3 "Hash Generator" p 3306 -- the RSIP HASH engine, reached
/// only through `ra8_rsip_sha256`, which carries its own register citations.
extern fn ra8_rsip_sha256(msg: [*]const u8, msg_len: u32, digest: [*]u8) u16;

/// The provisioned NIST P-256 root public key, `0x04 || X(32) || Y(32)`
/// (SEC1 v2 Sec 2.3.3), from the signing-key ceremony
/// (`scripts/secrets/rot_provision.sh`). The matching private key is held out
/// of tree and signs every launched image via `scripts/secrets/rot_sign.py`.
/// Public-key SHA-256 fingerprint:
/// e7080738d869454f203979242990e7982209dfc40485fd870b859478cecf4ac0.
/// Re-key by re-running the ceremony and replacing these bytes.
const root_public_key = [rot.Size.pubkey_bytes]u8{
    0x04, 0xC8, 0xDC, 0xA2, 0xF2, 0x02, 0x50, 0x15, 0xF2, 0xFE, 0x39, 0xD1, 0xBD,
    0x9A, 0xB9, 0xAF, 0x14, 0x6A, 0x76, 0xA6, 0x26, 0x67, 0x1D, 0xE7, 0xFD, 0xDA,
    0x53, 0x11, 0xF6, 0xEA, 0xC5, 0x85, 0xE6, 0x8E, 0x53, 0x0D, 0x1E, 0x52, 0x45,
    0xBB, 0x37, 0x96, 0xA4, 0xF1, 0x8C, 0xFA, 0x83, 0x22, 0x43, 0x0B, 0xAE, 0x74,
    0xD4, 0xB5, 0x53, 0xE5, 0xCD, 0xC7, 0x94, 0xBA, 0x57, 0x49, 0x22, 0x94, 0xDD,
};

fn digestOf(body: []const u8, out: *[rot.Size.digest_bytes]u8) u16 {
    if (off_target) {
        var produced: usize = 0;
        return ra8_psa_hash_compute(
            Psa.alg_sha_256,
            body.ptr,
            body.len,
            out,
            rot.Size.digest_bytes,
            &produced,
        );
    }
    return ra8_rsip_sha256(body.ptr, @intCast(body.len), out);
}

/// Import the root key with verify-only usage, check the signature, and
/// destroy the transient key on every path.
fn verifySignature(material: []const u8, sig: []const u8) u16 {
    const attr = KeyAttr{
        .type = Psa.key_type_ecc_p256_pub,
        .alg = Psa.alg_ecdsa_sha_256,
        .usage = Psa.usage_verify,
    };
    var key: ?*anyopaque = null;
    const import_status = ra8_psa_key_import(&key, &attr, &root_public_key, root_public_key.len);
    if (import_status != Err.ok) {
        ra8_log_emit_error(tag, "rot: import root key failed");
        return import_status;
    }
    const status = ra8_psa_verify_hash(
        key,
        Psa.alg_ecdsa_sha_256,
        material.ptr,
        material.len,
        sig.ptr,
        sig.len,
    );
    _ = ra8_psa_key_destroy(key);
    return status;
}

export fn ra8_rot_verify_image(
    body: ?[*]const u8,
    body_len: u32,
    trailer: ?*const rot.Trailer,
) u16 {
    const bytes = body orelse {
        ra8_log_emit_error(tag, "rot: body is NULL");
        return Err.null_ptr;
    };
    const record = trailer orelse {
        ra8_log_emit_error(tag, "rot: trailer is NULL");
        return Err.null_ptr;
    };

    switch (rot.screen(record, body_len)) {
        .ok => {},
        .malformed => {
            ra8_log_emit_error(tag, "rot: trailer magic/version invalid");
            return Err.validation_failed;
        },
        .bad_body_len => {
            ra8_log_emit_error(tag, "rot: body_len out of range / mismatch");
            return Err.invalid_size;
        },
        .bad_sig_len => {
            ra8_log_emit_error(tag, "rot: sig_len invalid");
            return Err.invalid_size;
        },
    }

    // The fake hash and the ECDSA verify both route through the PSA facade.
    // Already-initialized is fine.
    const psa_status = ra8_psa_crypto_init();
    if (psa_status != Err.ok and psa_status != Err.exists) {
        ra8_log_emit_error(tag, "rot: psa init failed");
        return psa_status;
    }

    var digest: [rot.Size.digest_bytes]u8 = undefined;
    const hash_status = digestOf(bytes[0..body_len], &digest);
    if (hash_status != Err.ok) {
        ra8_log_emit_error(tag, "rot: hash compute failed");
        return hash_status;
    }

    // Tamper pre-check against the trailer's stored digest.
    if (!rot.equalConstantTime(&digest, &record.digest)) {
        ra8_log_emit_error(tag, "rot: body digest mismatch (tampered)");
        return Err.checksum_mismatch;
    }

    // Authority: ECDSA-P256 over SHA-256(img_version_le || body_digest), so a
    // forged version on a validly-signed body fails to verify.
    const material = rot.signedMaterial(record.img_version, digest);
    var bound: [rot.Size.digest_bytes]u8 = undefined;
    const bind_status = digestOf(&material, &bound);
    if (bind_status != Err.ok) {
        ra8_log_emit_error(tag, "rot: version-bind hash failed");
        return bind_status;
    }

    const sig_status = verifySignature(&bound, record.sig[0..record.sig_len]);
    if (sig_status != Err.ok) {
        ra8_log_emit_error(tag, "rot: signature verify failed");
        return if (sig_status == Err.ok) Err.crc_mismatch else sig_status;
    }
    return Err.ok;
}

export fn ra8_rot_trailer_after(image_base: ?*const anyopaque, body_len: u32) ?*const rot.Trailer {
    const base = image_base orelse return null;
    const offset = rot.trailerOffset(body_len) orelse return null;
    // Image bodies are placed at 32-byte (page) multiples, so this address
    // satisfies the trailer's alignment.
    const bytes: [*]const u8 = @ptrCast(base);
    return @ptrCast(@alignCast(bytes + offset));
}

/// The version an authenticated trailer records, for the anti-rollback gate.
/// The trailer's layout stays inside this library: callers across an archive
/// boundary hold it as an opaque pointer.
export fn ra8_rot_trailer_image_version(trailer: ?*const rot.Trailer) u32 {
    const record = trailer orelse return 0;
    return record.img_version;
}

export fn ra8_rot_root_public_key(out_key: ?*?[*]const u8, out_len: ?*u32) u16 {
    const key_out = out_key orelse {
        ra8_log_emit_error(tag, "rot: out_key is NULL");
        return Err.null_ptr;
    };
    const len_out = out_len orelse {
        ra8_log_emit_error(tag, "rot: out_len is NULL");
        return Err.null_ptr;
    };
    key_out.* = &root_public_key;
    len_out.* = rot.Size.pubkey_bytes;
    return Err.ok;
}
