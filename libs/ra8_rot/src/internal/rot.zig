//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The root-of-trust decisions, with none of the cryptography in them.
//!
//! What a signed image is (`[ body ][ Trailer ]`), which trailers are
//! well-formed, how the anti-rollback version is bound into the signed
//! material, and how two digests are compared without leaking where they
//! first differ. The hash and the ECDSA verify live behind the C seam in
//! `rot_abi.zig`; everything here is decidable with no engine at all, which
//! is what lets the suite below drive it.

const std = @import("std");

/// Fixed byte lengths of the cryptographic fields in a signed image.
/// SHA-256 digest (FIPS 180-4), raw `r || s` ECDSA-P256 signature
/// (FIPS 186-4), uncompressed P-256 public key `0x04 || X || Y`
/// (SEC1 v2 Sec 2.3.3).
pub const Size = struct {
    pub const digest_bytes: u32 = 32;
    pub const sig_bytes: u32 = 64;
    pub const pubkey_bytes: u32 = 65;
};

/// Trailer magic, format version, and the body-length sanity cap. The 1 MiB
/// cap exceeds both the largest DFU slot image and the Non-Secure MRAM
/// partition while bounding the hash loop, so a corrupt `body_len` cannot
/// drive an unbounded read.
pub const Format = struct {
    pub const trailer_magic: u32 = 0x524F5431; // "ROT1"
    pub const version: u32 = 1;
    pub const body_max: u32 = 0x0010_0000;
};

/// The authenticity trailer that follows a signed image body. Laid out to
/// match `ra8_rot_trailer_t` in `inc/ra8_rot.h`; the comptime block below is
/// the Zig half of that header's `static_assert`.
pub const Trailer = extern struct {
    magic: u32,
    version: u32,
    img_version: u32,
    body_len: u32,
    sig_len: u32,
    digest: [Size.digest_bytes]u8,
    sig: [Size.sig_bytes]u8,
};

comptime {
    const want = (5 * @sizeOf(u32)) + Size.digest_bytes + Size.sig_bytes;
    if (@sizeOf(Trailer) != want) @compileError("ra8_rot_trailer_t must have no implicit padding");
    if (@offsetOf(Trailer, "digest") != 5 * @sizeOf(u32)) @compileError("digest must follow the five metadata words");
    if (@offsetOf(Trailer, "sig") != (5 * @sizeOf(u32)) + Size.digest_bytes) @compileError("sig must follow the digest");
}

/// Why a trailer was refused before any hashing happened. Every value other
/// than `ok` is a default-deny: the caller must not launch.
pub const Verdict = enum {
    ok,
    /// Magic or format version wrong: a missing or malformed trailer.
    malformed,
    /// `body_len` zero, past the cap, or disagreeing with the trailer.
    bad_body_len,
    /// `sig_len` zero or wider than an ECDSA-P256 signature.
    bad_sig_len,
};

/// Screen a trailer's own fields against the body it claims to cover. Pure:
/// it reads no image bytes and runs no engine, so the whole default-deny
/// decision table is reachable without crypto.
pub fn screen(trailer: *const Trailer, body_len: u32) Verdict {
    if (trailer.magic != Format.trailer_magic or trailer.version != Format.version) {
        return .malformed;
    }
    if (body_len == 0 or body_len > Format.body_max or trailer.body_len != body_len) {
        return .bad_body_len;
    }
    if (trailer.sig_len == 0 or trailer.sig_len > Size.sig_bytes) {
        return .bad_sig_len;
    }
    return .ok;
}

/// Byte offset of the trailer for a body of `body_len`, or null when the
/// length is outside `(0, body_max]`. The layout contract stated once, so
/// callers express it instead of open-coding a pointer add.
pub fn trailerOffset(body_len: u32) ?u32 {
    if (body_len == 0 or body_len > Format.body_max) return null;
    return body_len;
}

/// Constant-time equality: folds the XOR of every byte pair into an
/// accumulator so the time does not depend on where the first difference
/// sits. Empty and length-mismatched inputs are refused rather than
/// trivially equal, which keeps a zero-length compare from reading as a
/// match.
pub fn equalConstantTime(a: []const u8, b: []const u8) bool {
    if (a.len == 0 or a.len != b.len) return false;
    var diff: u8 = 0;
    for (a, b) |lhs, rhs| diff |= lhs ^ rhs;
    return diff == 0;
}

/// The bytes the signature is actually asserted over: the little-endian
/// `img_version` followed by the body digest. Binding the version in means a
/// forged `img_version` riding on a validly-signed body no longer verifies,
/// so an attacker holding an older signed image cannot raise the field to
/// defeat anti-rollback. The signer `scripts/secrets/rot_sign.py` builds the
/// identical material.
pub fn signedMaterial(img_version: u32, digest: [Size.digest_bytes]u8) [@sizeOf(u32) + Size.digest_bytes]u8 {
    var combined: [@sizeOf(u32) + Size.digest_bytes]u8 = undefined;
    std.mem.writeInt(u32, combined[0..@sizeOf(u32)], img_version, .little);
    @memcpy(combined[@sizeOf(u32)..], &digest);
    return combined;
}
