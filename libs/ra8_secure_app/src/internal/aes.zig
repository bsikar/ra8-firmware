//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! AES-128 / AES-256 block encryption, FIPS 197, and nothing else.
//!
//! This is the compact reference shape the C carried: one 256-byte S-box and
//! the round transforms computed on the fly, rather than the four 1 KiB
//! T-tables a speed-first implementation uses. The secure-side archive is
//! linked into every app, so the .rodata footprint matters more here than
//! throughput: CMAC authenticates a wrapped-key blob of at most 256 bytes,
//! once, at import time.
//!
//! Encryption only. CMAC never needs the inverse cipher, so there is no
//! decrypt path to keep correct or to test.

const std = @import("std");

/// AES dimensions, FIPS 197 Sec 5.
pub const Dim = struct {
    /// Block size, which is also the CMAC tag length.
    pub const block_bytes: usize = 16;
    /// Bytes per key-schedule word.
    pub const word_bytes: usize = 4;
    /// Columns in the state.
    pub const state_cols: usize = 4;
    /// Round-key bytes for AES-256: 16 * (14 + 1).
    pub const max_round_key_bytes: usize = 240;
};

/// The GF(2^8) reduction polynomial, FIPS 197 Sec 4.2.
pub const field_poly: u8 = 0x1b;

/// One AES block.
pub const Block = [Dim.block_bytes]u8;

/// The key lengths this cipher accepts.
pub const KeyLen = enum {
    aes_128,
    aes_256,

    /// Key words (Nk): 4 for AES-128, 8 for AES-256.
    pub fn words(self: KeyLen) usize {
        return switch (self) {
            .aes_128 => 4,
            .aes_256 => 8,
        };
    }

    /// Round count (Nr): 10 for AES-128, 14 for AES-256.
    pub fn rounds(self: KeyLen) usize {
        return switch (self) {
            .aes_128 => 10,
            .aes_256 => 14,
        };
    }

    /// Key length in bytes.
    pub fn bytes(self: KeyLen) usize {
        return self.words() * Dim.word_bytes;
    }

    /// The `KeyLen` for a raw key of `len` bytes, or null when `len` is
    /// neither 16 nor 32.
    pub fn fromBytes(len: usize) ?KeyLen {
        return switch (len) {
            16 => .aes_128,
            32 => .aes_256,
            else => null,
        };
    }
};

/// The FIPS 197 Fig. 7 substitution table.
const sbox = [256]u8{
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

/// Multiply a byte by x in GF(2^8), FIPS 197 Sec 4.2.
pub fn xtime(value: u8) u8 {
    const carry: u8 = if ((value & 0x80) != 0) field_poly else 0;
    return (value << 1) ^ carry;
}

/// An expanded key schedule, valid for one key length.
///
/// The buffer is sized for AES-256 whichever length is in use; `used` is the
/// live prefix, so the unused tail of an AES-128 schedule is never read.
pub const Schedule = struct {
    bytes: [Dim.max_round_key_bytes]u8,
    key_len: KeyLen,

    /// Expand `key` into the full round-key schedule, FIPS 197 Sec 5.2.
    ///
    /// `key.len` selects the variant and must be 16 or 32; any other length is
    /// a caller bug, not a runtime outcome, so it asserts.
    pub fn init(key: []const u8) Schedule {
        const key_len = KeyLen.fromBytes(key.len) orelse unreachable;
        var self = Schedule{ .bytes = undefined, .key_len = key_len };

        const nk = key_len.words();
        const total_words = Dim.state_cols * (key_len.rounds() + 1);
        @memcpy(self.bytes[0..key.len], key);

        var rcon: u8 = 1;
        var i: usize = nk;
        while (i < total_words) : (i += 1) {
            var t: [Dim.word_bytes]u8 = undefined;
            @memcpy(&t, self.bytes[(i - 1) * Dim.word_bytes ..][0..Dim.word_bytes]);

            if (i % nk == 0) {
                const first = t[0];
                t[0] = sbox[t[1]] ^ rcon;
                t[1] = sbox[t[2]];
                t[2] = sbox[t[3]];
                t[3] = sbox[first];
                rcon = xtime(rcon);
            } else if (nk > 6 and i % nk == 4) {
                // AES-256 only: the extra SubWord a quarter of the way through
                // each key block.
                for (&t) |*byte| byte.* = sbox[byte.*];
            }

            const prev = self.bytes[(i - nk) * Dim.word_bytes ..][0..Dim.word_bytes];
            const out = self.bytes[i * Dim.word_bytes ..][0..Dim.word_bytes];
            for (out, prev, t) |*dst, a, b| dst.* = a ^ b;
        }
        return self;
    }

    /// The round keys actually in use, `16 * (rounds + 1)` bytes.
    pub fn used(self: *const Schedule) []const u8 {
        return self.bytes[0 .. Dim.block_bytes * (self.key_len.rounds() + 1)];
    }

    /// Wipe the schedule. Round keys recover the key, so a caller that is done
    /// with one clears it rather than letting it sit on the stack.
    pub fn deinit(self: *Schedule) void {
        std.crypto.secureZero(u8, &self.bytes);
    }
};

/// SubBytes then ShiftRows, in place, FIPS 197 Sec 5.1.1 and 5.1.2.
///
/// The state is column-major: byte `4*c + r` is row `r` of column `c`.
fn subShift(state: *Block) void {
    for (state) |*byte| byte.* = sbox[byte.*];

    var shifted: Block = undefined;
    for (0..Dim.state_cols) |r| {
        for (0..Dim.state_cols) |c| {
            shifted[(c * Dim.state_cols) + r] = state[(((c + r) & 3) * Dim.state_cols) + r];
        }
    }
    state.* = shifted;
}

/// MixColumns, in place, FIPS 197 Sec 5.1.3.
fn mixColumns(state: *Block) void {
    for (0..Dim.state_cols) |c| {
        const col = state[c * Dim.state_cols ..][0..Dim.state_cols];
        const a0 = col[0];
        const a1 = col[1];
        const a2 = col[2];
        const a3 = col[3];
        col[0] = xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3;
        col[1] = a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3;
        col[2] = a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3;
        col[3] = xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3);
    }
}

/// Encrypt one block under `schedule`, FIPS 197 Sec 5.1.
pub fn encryptBlock(schedule: *const Schedule, in: Block) Block {
    const rk = schedule.used();
    const rounds = schedule.key_len.rounds();

    var state: Block = undefined;
    for (&state, in, rk[0..Dim.block_bytes]) |*dst, a, b| dst.* = a ^ b;

    var round: usize = 1;
    while (round <= rounds) : (round += 1) {
        subShift(&state);
        if (round != rounds) mixColumns(&state);
        const round_key = rk[round * Dim.block_bytes ..][0..Dim.block_bytes];
        for (&state, round_key) |*dst, k| dst.* ^= k;
    }
    return state;
}
