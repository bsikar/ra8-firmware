//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Off-target crypto stand-ins. None of this is secure and none of it ever
//! reaches silicon: it exists so the host suite can exercise the AEAD
//! contract (ciphertext||tag layout, tamper detection) and a reproducible
//! entropy stream without linking TF-PSA-Crypto.
//!
//! The C spelled SHA-256 out longhand as a FIPS 180-4 reference; `std.crypto`
//! is the same function, so the port calls that instead of carrying a second
//! copy of the compression rounds.

const std = @import("std");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Limits = vocab.Limits;

/// Scratch budget shared by the keystream and tag helpers. Matches the C's
/// `k_ra8_psa_fake_scratch_bytes`, which the host AEAD tests pin.
pub const scratch_bytes: usize = 256;

pub fn sha256(input: []const u8, out: *[Limits.sha256_len]u8) void {
    std.crypto.hash.sha2.Sha256.hash(input, out, .{});
}

/// Append as much of `src` as still fits, exactly as the C's bounded
/// `while (i < len && off < sizeof buf)` copies did. Truncation is the
/// documented behaviour, not an error.
fn appendClamped(buffer: []u8, offset: usize, src: []const u8) usize {
    const room = buffer.len - offset;
    const take = @min(room, src.len);
    @memcpy(buffer[offset..][0..take], src[0..take]);
    return offset + take;
}

/// Tag = SHA-256(key || nonce || aad || cipher) truncated to 16 bytes.
pub fn tag(
    key: []const u8,
    nonce: []const u8,
    aad: []const u8,
    cipher: []const u8,
    out: *[Limits.gcm_tag_len]u8,
) void {
    var buffer: [scratch_bytes]u8 = undefined;
    var offset: usize = 0;
    offset = appendClamped(&buffer, offset, key);
    offset = appendClamped(&buffer, offset, nonce);
    offset = appendClamped(&buffer, offset, aad);
    offset = appendClamped(&buffer, offset, cipher);

    var digest: [Limits.sha256_len]u8 = undefined;
    sha256(buffer[0..offset], &digest);
    out.* = digest[0..Limits.gcm_tag_len].*;
}

/// Counter-mode keystream over SHA-256(key || nonce || be32(counter)).
pub fn keystream(key: []const u8, nonce: []const u8, destination: []u8) void {
    var seed: [scratch_bytes]u8 = undefined;
    var offset: usize = 0;
    offset = appendClamped(&seed, offset, key);
    offset = appendClamped(&seed, offset, nonce);
    std.debug.assert(offset + @sizeOf(u32) <= seed.len);

    var block: [Limits.sha256_len]u8 = undefined;
    var counter: u32 = 0;
    var produced: usize = 0;
    while (produced < destination.len) : (counter += 1) {
        std.mem.writeInt(u32, seed[offset..][0..4], counter, .big);
        sha256(seed[0 .. offset + @sizeOf(u32)], &block);
        const take = @min(destination.len - produced, block.len);
        @memcpy(destination[produced..][0..take], block[0..take]);
        produced += take;
    }
}

/// Encrypt = plaintext XOR keystream, then append the tag over the ciphertext.
/// `out` must hold `plain.len + Limits.gcm_tag_len` bytes.
pub fn aeadEncrypt(
    key: []const u8,
    nonce: []const u8,
    aad: []const u8,
    plain: []const u8,
    out: []u8,
) u16 {
    if (plain.len > scratch_bytes) return Err.invalid_size;
    if (plain.len != 0) {
        var pad: [scratch_bytes]u8 = undefined;
        keystream(key, nonce, pad[0..plain.len]);
        for (out[0..plain.len], plain, pad[0..plain.len]) |*dst, src, mask| {
            dst.* = src ^ mask;
        }
    }
    var computed: [Limits.gcm_tag_len]u8 = undefined;
    tag(key, nonce, aad, out[0..plain.len], &computed);
    @memcpy(out[plain.len..][0..Limits.gcm_tag_len], &computed);
    return Err.ok;
}

/// Verify the trailing tag, then recover the plaintext. `cipher` is
/// ciphertext||tag and `out` holds `cipher.len - Limits.gcm_tag_len` bytes.
pub fn aeadDecrypt(
    key: []const u8,
    nonce: []const u8,
    aad: []const u8,
    cipher: []const u8,
    out: []u8,
) u16 {
    const plain_len = cipher.len - Limits.gcm_tag_len;
    var expected: [Limits.gcm_tag_len]u8 = undefined;
    tag(key, nonce, aad, cipher[0..plain_len], &expected);
    if (!std.crypto.timing_safe.eql(
        [Limits.gcm_tag_len]u8,
        expected,
        cipher[plain_len..][0..Limits.gcm_tag_len].*,
    )) return Err.crc_mismatch;

    if (plain_len > scratch_bytes) return Err.invalid_size;
    if (plain_len != 0) {
        var pad: [scratch_bytes]u8 = undefined;
        keystream(key, nonce, pad[0..plain_len]);
        for (out[0..plain_len], cipher[0..plain_len], pad[0..plain_len]) |*dst, src, mask| {
            dst.* = src ^ mask;
        }
    }
    return Err.ok;
}

/// xorshift32 (Marsaglia 2003, row y[13,17,5]). The C held this state in a
/// function-local `static`, so it deliberately survives init/deinit cycles
/// and the host tests see one continuous stream per process.
pub const Rng = struct {
    state: u32 = 0xACE1ACE1,

    pub fn fill(self: *Rng, destination: []u8) void {
        for (destination) |*byte| {
            var x = self.state;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            self.state = x;
            byte.* = @truncate(x);
        }
    }
};
