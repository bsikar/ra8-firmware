//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! AES-CMAC, NIST SP 800-38B, and nothing else.
//!
//! The secure-side key importer authenticates a wrapped-key blob with this
//! before the blob is admitted to the vault. The CMAC key is the
//! key-authentication key the importer loads from the vault, so it never
//! originates anywhere the Non-Secure world can reach.
//!
//! `msg` is capped at `Limits.max_msg_bytes` so every loop here has a bound a
//! reader can check (NASA Power of 10 Rule 2), and so the caller cannot hand
//! the secure side an unbounded walk.

const std = @import("std");

const aes = @import("aes.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;

/// Sizing constants, mirroring `src/sec_cmac_internal.h`.
pub const Limits = struct {
    /// Tag length: one AES block.
    pub const tag_bytes: usize = aes.Dim.block_bytes;
    /// AES-128 key length.
    pub const key_128: usize = 16;
    /// AES-256 key length.
    pub const key_256: usize = 32;
    /// Largest message this seam will authenticate.
    pub const max_msg_bytes: usize = 256;
};

/// The GF(2^128) constant Rb, SP 800-38B Sec 5.3.
const rb: u8 = 0x87;

/// The final-block pad marker, SP 800-38B Sec 5.5.
const pad_marker: u8 = 0x80;

/// One CMAC tag.
pub const Tag = [Limits.tag_bytes]u8;

/// Shared precondition check for both entry points, so compute and verify
/// cannot drift apart on what they accept.
pub fn checkArgs(key: []const u8, msg: []const u8) Err {
    if (aes.KeyLen.fromBytes(key.len) == null) return .invalid_arg;
    if (msg.len > Limits.max_msg_bytes) return .invalid_size;
    return .ok;
}

/// Double a 128-bit value in GF(2^128), the CMAC subkey step,
/// SP 800-38B Sec 6.1.
fn double(in: aes.Block) aes.Block {
    const msb = in[0] >> 7;
    var out: aes.Block = undefined;
    for (0..aes.Dim.block_bytes - 1) |i| {
        out[i] = (in[i] << 1) | (in[i + 1] >> 7);
    }
    out[aes.Dim.block_bytes - 1] = in[aes.Dim.block_bytes - 1] << 1;
    if (msb != 0) out[aes.Dim.block_bytes - 1] ^= rb;
    return out;
}

/// The two CMAC subkeys, SP 800-38B Sec 6.1.
const Subkeys = struct {
    /// For a final block that is a whole 16 bytes.
    complete: aes.Block,
    /// For a padded final block.
    padded: aes.Block,

    /// `L = AES(key, 0^128)`, `K1 = 2L`, `K2 = 2 * K1`.
    ///
    /// `L` recovers both subkeys, so it is wiped before this returns.
    fn derive(schedule: *const aes.Schedule) Subkeys {
        var l = aes.encryptBlock(schedule, @as([aes.Dim.block_bytes]u8, @splat(0)));
        defer std.crypto.secureZero(u8, &l);
        const k1 = double(l);
        return .{ .complete = k1, .padded = double(k1) };
    }
};

/// Build the final block: XOR with K1 when it is complete, otherwise pad with
/// `0x80 0x00...` and XOR with K2. SP 800-38B Sec 6.2 step 4.
fn buildLast(tail: []const u8, keys: Subkeys) aes.Block {
    if (tail.len == aes.Dim.block_bytes) {
        var last: aes.Block = undefined;
        for (&last, tail, keys.complete) |*dst, m, k| dst.* = m ^ k;
        return last;
    }
    var last: aes.Block = undefined;
    for (&last, keys.padded, 0..) |*dst, k, i| {
        const byte: u8 = if (i < tail.len) tail[i] else if (i == tail.len) pad_marker else 0;
        dst.* = byte ^ k;
    }
    return last;
}

/// Compute the CMAC tag of `msg` under `key`, SP 800-38B Sec 6.2.
///
/// An empty message is one padded block, per the standard. `key.len` must be
/// 16 or 32 and `msg.len` within `Limits.max_msg_bytes`: run `checkArgs`
/// first, since a bad key length is unreachable here.
pub fn tag(key: []const u8, msg: []const u8) Tag {
    var schedule = aes.Schedule.init(key);
    defer schedule.deinit();

    var keys = Subkeys.derive(&schedule);
    defer {
        std.crypto.secureZero(u8, &keys.complete);
        std.crypto.secureZero(u8, &keys.padded);
    }

    // Block count, and where the final block starts. An empty message still
    // has one (padded) block.
    const block_count = if (msg.len == 0)
        1
    else
        (msg.len + aes.Dim.block_bytes - 1) / aes.Dim.block_bytes;
    const last_off = (block_count - 1) * aes.Dim.block_bytes;
    const last = buildLast(msg[last_off..], keys);

    var x: [aes.Dim.block_bytes]u8 = @splat(0);
    var b: usize = 0;
    while (b + 1 < block_count) : (b += 1) {
        var y: aes.Block = undefined;
        const block = msg[b * aes.Dim.block_bytes ..][0..aes.Dim.block_bytes];
        for (&y, x, block) |*dst, chain, m| dst.* = chain ^ m;
        x = aes.encryptBlock(&schedule, y);
    }

    var y: aes.Block = undefined;
    for (&y, x, last) |*dst, chain, m| dst.* = chain ^ m;
    return aes.encryptBlock(&schedule, y);
}

/// Compute the tag of `msg` into `out`.
pub fn compute(key: []const u8, msg: []const u8, out: *Tag) Err {
    const check = checkArgs(key, msg);
    if (check != .ok) return check;
    out.* = tag(key, msg);
    return .ok;
}

/// Verify `mac` against `msg` under `key`.
///
/// The verdict is this module's security-critical decision, so it is taken in
/// two parts that are both wrong the same way: a tag of the wrong length, and
/// a tag that does not match. The comparison has no data-dependent early-out,
/// so its timing does not leak how many leading bytes matched.
pub fn verify(key: []const u8, msg: []const u8, mac: []const u8) Err {
    const check = checkArgs(key, msg);
    if (check != .ok) return check;

    var computed = tag(key, msg);
    defer std.crypto.secureZero(u8, &computed);

    const length_bad = mac.len != Limits.tag_bytes;
    // A wrong-length tag cannot be compared, so compare against the computed
    // tag itself: the work is identical either way and the verdict is already
    // decided by `length_bad`.
    const candidate = if (length_bad) &computed else mac[0..Limits.tag_bytes];
    const tag_bad = !std.crypto.timing_safe.eql([Limits.tag_bytes]u8, computed, candidate.*);

    if (length_bad or tag_bad) return .invalid_arg;
    return .ok;
}
