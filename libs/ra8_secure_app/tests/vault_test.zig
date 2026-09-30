//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The vault's digest is the one thing that crosses to the Non-Secure world, so
//! the known-answer vectors below are the load-bearing test. They were produced
//! by the C this file replaces (`internal_sha256_32`, driven through
//! `ra8_key_vault_sha256_xor_challenge` with a zero challenge) and independently
//! confirmed against a reference SHA-256. If the `std.crypto` swap ever drifts
//! from the C, these fail.

const std = @import("std");
const vault = @import("vault");

const zero_challenge: [32]u8 = .{0} ** 32;

fn hex(comptime s: *const [64:0]u8) [32]u8 {
    var out: [32]u8 = undefined;
    _ = std.fmt.hexToBytes(&out, s) catch unreachable;
    return out;
}

fn digestOf(key: [32]u8) ![32]u8 {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    try std.testing.expectEqual(vault.Err.ok, vault.store(0, &key));
    var out: [32]u8 = undefined;
    try std.testing.expectEqual(vault.Err.ok, vault.sha256XorChallenge(0, &zero_challenge, &out));
    return out;
}

test "known-answer vectors carried over from the C implementation" {
    var ascending: [32]u8 = undefined;
    for (&ascending, 0..) |*b, i| b.* = @intCast(i);
    var strided: [32]u8 = undefined;
    for (&strided, 0..) |*b, i| b.* = @truncate((i * 7) + 3);

    try std.testing.expectEqual(
        hex("66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925"),
        try digestOf(.{0} ** 32),
    );
    try std.testing.expectEqual(
        hex("af9613760f72635fbdb44a5a0a63c39f12af30f950a6ee5c971be188e89c4051"),
        try digestOf(.{0xFF} ** 32),
    );
    try std.testing.expectEqual(
        hex("630dcd2966c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd"),
        try digestOf(ascending),
    );
    try std.testing.expectEqual(
        hex("ab5f8b5cb9435354c7b58603592d5faf081e17ceb05f7a7c67f4b666f12ca457"),
        try digestOf(strided),
    );
}

test "the challenge is mixed in, not ignored" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    try std.testing.expectEqual(vault.Err.ok, vault.store(0, &(.{0xA5} ** 32)));

    var with_zero: [32]u8 = undefined;
    var with_ones: [32]u8 = undefined;
    try std.testing.expectEqual(
        vault.Err.ok,
        vault.sha256XorChallenge(0, &zero_challenge, &with_zero),
    );
    try std.testing.expectEqual(
        vault.Err.ok,
        vault.sha256XorChallenge(0, &(.{0xFF} ** 32), &with_ones),
    );
    try std.testing.expect(!std.mem.eql(u8, &with_zero, &with_ones));
}

test "a zeroed slot XORed with a challenge hashes the challenge itself" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    var out: [32]u8 = undefined;
    try std.testing.expectEqual(vault.Err.ok, vault.sha256XorChallenge(3, &(.{0} ** 32), &out));
    try std.testing.expectEqual(
        hex("66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925"),
        out,
    );
}

test "slot index is bounds-checked on both entry points" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    try std.testing.expectEqual(vault.Err.invalid_arg, vault.store(vault.Limits.slots, &(.{1} ** 32)));
    var out: [32]u8 = undefined;
    try std.testing.expectEqual(
        vault.Err.invalid_arg,
        vault.sha256XorChallenge(vault.Limits.slots, &zero_challenge, &out),
    );
    // The last valid slot is still reachable.
    try std.testing.expectEqual(vault.Err.ok, vault.store(vault.Limits.slots - 1, &(.{1} ** 32)));
}

test "slots do not alias each other" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    try std.testing.expectEqual(vault.Err.ok, vault.store(0, &(.{0x11} ** 32)));
    try std.testing.expectEqual(vault.Err.ok, vault.store(7, &(.{0x22} ** 32)));

    var first: [32]u8 = undefined;
    var last: [32]u8 = undefined;
    try std.testing.expectEqual(vault.Err.ok, vault.sha256XorChallenge(0, &zero_challenge, &first));
    try std.testing.expectEqual(vault.Err.ok, vault.sha256XorChallenge(7, &zero_challenge, &last));
    try std.testing.expect(!std.mem.eql(u8, &first, &last));
}

test "KAK round-trips at both accepted lengths" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());

    const key128 = [_]u8{0xC3} ** 16;
    try std.testing.expectEqual(vault.Err.ok, vault.setMacKey(&key128));
    var out: [32]u8 = undefined;
    var out_len: u16 = 0;
    try std.testing.expectEqual(vault.Err.ok, vault.loadMacKey(&out, &out_len));
    try std.testing.expectEqual(@as(u16, 16), out_len);
    try std.testing.expectEqualSlices(u8, &key128, out[0..out_len]);

    const key256 = [_]u8{0x5A} ** 32;
    try std.testing.expectEqual(vault.Err.ok, vault.setMacKey(&key256));
    try std.testing.expectEqual(vault.Err.ok, vault.loadMacKey(&out, &out_len));
    try std.testing.expectEqual(@as(u16, 32), out_len);
    try std.testing.expectEqualSlices(u8, &key256, out[0..out_len]);
}

test "KAK rejects a length that is neither AES-128 nor AES-256" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    try std.testing.expectEqual(vault.Err.invalid_arg, vault.setMacKey(&(.{0} ** 24)));
    try std.testing.expectEqual(vault.Err.invalid_arg, vault.setMacKey(&(.{0} ** 15)));
    try std.testing.expectEqual(vault.Err.invalid_arg, vault.setMacKey(&.{}));
}

test "loading the KAK before provisioning reports not_found" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    var out: [32]u8 = undefined;
    var out_len: u16 = 0;
    try std.testing.expectEqual(vault.Err.not_found, vault.loadMacKey(&out, &out_len));
}

test "a too-small destination reports invalid_size and copies nothing" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    try std.testing.expectEqual(vault.Err.ok, vault.setMacKey(&(.{0x7E} ** 32)));

    var small: [16]u8 = .{0} ** 16;
    var out_len: u16 = 0;
    try std.testing.expectEqual(vault.Err.invalid_size, vault.loadMacKey(&small, &out_len));
    try std.testing.expectEqualSlices(u8, &(.{0} ** 16), &small);
    try std.testing.expectEqual(@as(u16, 0), out_len);
}

test "a shorter KAK leaves no tail of the longer one behind" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    try std.testing.expectEqual(vault.Err.ok, vault.setMacKey(&(.{0xFF} ** 32)));
    try std.testing.expectEqual(vault.Err.ok, vault.setMacKey(&(.{0x01} ** 16)));

    var out: [32]u8 = .{0} ** 32;
    var out_len: u16 = 0;
    try std.testing.expectEqual(vault.Err.ok, vault.loadMacKey(&out, &out_len));
    try std.testing.expectEqual(@as(u16, 16), out_len);
    try std.testing.expectEqualSlices(u8, &(.{0x01} ** 16), out[0..16]);
}

test "init drops both the slots and the KAK" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    try std.testing.expectEqual(vault.Err.ok, vault.store(2, &(.{0x9B} ** 32)));
    try std.testing.expectEqual(vault.Err.ok, vault.setMacKey(&(.{0x9B} ** 16)));

    try std.testing.expectEqual(vault.Err.ok, vault.init());

    var out: [32]u8 = undefined;
    var out_len: u16 = 0;
    try std.testing.expectEqual(vault.Err.not_found, vault.loadMacKey(&out, &out_len));

    var digest: [32]u8 = undefined;
    try std.testing.expectEqual(vault.Err.ok, vault.sha256XorChallenge(2, &zero_challenge, &digest));
    try std.testing.expectEqual(
        hex("66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925"),
        digest,
    );
}
