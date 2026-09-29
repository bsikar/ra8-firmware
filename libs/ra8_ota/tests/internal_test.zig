//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the pure parsing half of `ra8_ota`: the string.h
//! replacements, the JSON scanners, the hex decoder, the numeric gates and the
//! two promoted MC/DC predicates.

const std = @import("std");
const impl = @import("implementation");

test "charInRange is a closed interval on both ends" {
    try std.testing.expect(impl.charInRange('5', '0', '9'));
    try std.testing.expect(impl.charInRange('0', '0', '9'));
    try std.testing.expect(impl.charInRange('9', '0', '9'));
    try std.testing.expect(!impl.charInRange('/', '0', '9'));
    try std.testing.expect(!impl.charInRange(':', '0', '9'));
}

test "downloadStateInvalid rejects only states that are neither idle nor downloading" {
    try std.testing.expect(!impl.downloadStateInvalid(0, 1, 0));
    try std.testing.expect(!impl.downloadStateInvalid(0, 1, 1));
    try std.testing.expect(impl.downloadStateInvalid(0, 1, 4));
}

test "strLen and strStr match the C library semantics" {
    try std.testing.expectEqual(@as(usize, 0), impl.strLen(""));
    try std.testing.expectEqual(@as(usize, 5), impl.strLen("abcde"));

    try std.testing.expectEqual(@as(?usize, 0), impl.strStr("abcde", "abc"));
    try std.testing.expectEqual(@as(?usize, 2), impl.strStr("abcde", "cd"));
    try std.testing.expectEqual(@as(?usize, null), impl.strStr("abcde", "cde!"));
    // An empty needle matches at offset zero, as strstr does.
    try std.testing.expectEqual(@as(?usize, 0), impl.strStr("abcde", ""));
    // A near miss must not stop the scan at the first partial match.
    try std.testing.expectEqual(@as(?usize, 1), impl.strStr("aaab", "aab"));
    try std.testing.expectEqual(@as(?usize, 0), impl.strStr("aabaab", "aab"));
}

test "chrFrom scans from an offset and finds the terminator for a zero needle" {
    try std.testing.expectEqual(@as(?usize, 1), impl.chrFrom("a\"b\"", 0, '"'));
    try std.testing.expectEqual(@as(?usize, 3), impl.chrFrom("a\"b\"", 2, '"'));
    try std.testing.expectEqual(@as(?usize, null), impl.chrFrom("abc", 0, '"'));
    try std.testing.expectEqual(@as(?usize, 3), impl.chrFrom("abc", 0, 0));
}

test "jsonStr copies the quoted value and NUL-terminates it" {
    var dst: [16]u8 = undefined;
    const json = "{\"version\": \"1.2.3\", \"url\": \"https://x\"}";

    try std.testing.expectEqual(impl.err.ok, impl.jsonStr(json, "\"version\"", dst[0..]));
    try std.testing.expectEqualStrings("1.2.3", std.mem.sliceTo(dst[0..], 0));

    try std.testing.expectEqual(impl.err.ok, impl.jsonStr(json, "\"url\"", dst[0..]));
    try std.testing.expectEqualStrings("https://x", std.mem.sliceTo(dst[0..], 0));
}

test "jsonStr refuses a missing key, a missing open quote and a missing close quote" {
    var dst: [16]u8 = undefined;
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        impl.jsonStr("{\"a\": \"b\"}", "\"zz\"", dst[0..]),
    );
    // Key present but no quote anywhere after it.
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        impl.jsonStr("version: 3", "version", dst[0..]),
    );
    // Open quote present, close quote missing.
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        impl.jsonStr("version \"3", "version", dst[0..]),
    );
}

test "jsonStr needs room for the value plus its NUL" {
    var exact: [6]u8 = undefined;
    try std.testing.expectEqual(
        impl.err.ok,
        impl.jsonStr("{\"v\": \"12345\"}", "\"v\"", exact[0..]),
    );
    try std.testing.expectEqualStrings("12345", std.mem.sliceTo(exact[0..], 0));

    var tight: [5]u8 = undefined;
    try std.testing.expectEqual(
        impl.err.invalid_size,
        impl.jsonStr("{\"v\": \"12345\"}", "\"v\"", tight[0..]),
    );
}

test "jsonU32 skips the colon/space/quote run and reads decimal digits" {
    var v: u32 = 0xDEAD;
    try std.testing.expectEqual(impl.err.ok, impl.jsonU32("{\"size\": 4096}", "\"size\"", &v));
    try std.testing.expectEqual(@as(u32, 4096), v);

    try std.testing.expectEqual(impl.err.ok, impl.jsonU32("{\"size\":\"77\"}", "\"size\"", &v));
    try std.testing.expectEqual(@as(u32, 77), v);
}

test "jsonU32 refuses a missing key and a key with no digits, leaving the out value alone" {
    var v: u32 = 0x1234;
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        impl.jsonU32("{\"size\": 1}", "\"other\"", &v),
    );
    try std.testing.expectEqual(@as(u32, 0x1234), v);

    try std.testing.expectEqual(
        impl.err.invalid_arg,
        impl.jsonU32("{\"size\": abc}", "\"size\"", &v),
    );
    try std.testing.expectEqual(@as(u32, 0x1234), v);
}

test "jsonU32 stops after twelve digits and wraps like the C uint32_t accumulate" {
    var v: u32 = 0;
    // Thirteen digits: only the first twelve are consumed.
    try std.testing.expectEqual(
        impl.err.ok,
        impl.jsonU32("{\"size\": 1234567890123}", "\"size\"", &v),
    );
    var expected: u32 = 0;
    for ("123456789012") |c| {
        expected = (expected *% 10) +% @as(u32, c - '0');
    }
    try std.testing.expectEqual(expected, v);
}

test "jsonU32 gives up skipping after eight separator characters" {
    var v: u32 = 0xAAAA;
    // Nine spaces before the digit: the bounded skip loop never reaches it.
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        impl.jsonU32("{\"size\":         7}", "\"size\"", &v),
    );
    try std.testing.expectEqual(@as(u32, 0xAAAA), v);

    // Eight separators is still within budget.
    var ok_v: u32 = 0;
    try std.testing.expectEqual(
        impl.err.ok,
        impl.jsonU32("{\"size\":       7}", "\"size\"", &ok_v),
    );
    try std.testing.expectEqual(@as(u32, 7), ok_v);
}

test "hexNibble maps both letter cases and rejects everything else" {
    try std.testing.expectEqual(@as(u8, 0), impl.hexNibble('0'));
    try std.testing.expectEqual(@as(u8, 9), impl.hexNibble('9'));
    try std.testing.expectEqual(@as(u8, 10), impl.hexNibble('a'));
    try std.testing.expectEqual(@as(u8, 15), impl.hexNibble('f'));
    try std.testing.expectEqual(@as(u8, 10), impl.hexNibble('A'));
    try std.testing.expectEqual(@as(u8, 15), impl.hexNibble('F'));
    try std.testing.expectEqual(impl.hex_invalid_nibble, impl.hexNibble('g'));
    try std.testing.expectEqual(impl.hex_invalid_nibble, impl.hexNibble('G'));
    try std.testing.expectEqual(impl.hex_invalid_nibble, impl.hexNibble(' '));
}

test "hexDecode returns the byte count and refuses odd, oversized or malformed input" {
    var out: [4]u8 = undefined;

    try std.testing.expectEqual(@as(u32, 3), impl.hexDecode("0aFF10", out[0..]));
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x0a, 0xff, 0x10 }, out[0..3]);

    // Odd length.
    try std.testing.expectEqual(@as(u32, 0), impl.hexDecode("0af", out[0..]));
    // Over capacity.
    try std.testing.expectEqual(@as(u32, 0), impl.hexDecode("0011223344", out[0..]));
    // Bad nibble in the high half and in the low half.
    try std.testing.expectEqual(@as(u32, 0), impl.hexDecode("zz11", out[0..]));
    try std.testing.expectEqual(@as(u32, 0), impl.hexDecode("11z1", out[0..]));
    // Empty input is even, fits, and decodes zero bytes.
    try std.testing.expectEqual(@as(u32, 0), impl.hexDecode("", out[0..]));
}

test "bankSizeStatus and manifestSizeStatus use different codes for the over-cap case" {
    try std.testing.expectEqual(impl.err.invalid_arg, impl.bankSizeStatus(0));
    try std.testing.expectEqual(impl.err.ok, impl.bankSizeStatus(1));
    try std.testing.expectEqual(impl.err.ok, impl.bankSizeStatus(impl.max_image_bytes));
    try std.testing.expectEqual(impl.err.invalid_arg, impl.bankSizeStatus(impl.max_image_bytes + 1));

    try std.testing.expectEqual(impl.err.invalid_arg, impl.manifestSizeStatus(0));
    try std.testing.expectEqual(impl.err.ok, impl.manifestSizeStatus(impl.max_image_bytes));
    try std.testing.expectEqual(
        impl.err.invalid_size,
        impl.manifestSizeStatus(impl.max_image_bytes + 1),
    );
}

test "manifestUrlEmpty only fires on a leading NUL" {
    try std.testing.expect(impl.manifestUrlEmpty(0));
    try std.testing.expect(!impl.manifestUrlEmpty('h'));
}

test "the C ABI struct layouts hold" {
    try std.testing.expectEqual(@as(usize, 424), @sizeOf(impl.Manifest));
    try std.testing.expectEqual(@as(usize, 288), @offsetOf(impl.Manifest, "image_size_bytes"));
    try std.testing.expectEqual(@as(usize, 420), @offsetOf(impl.Manifest, "signature_len"));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(impl.Cfg, "manifest_url"));
    try std.testing.expectEqual(@as(usize, 256), @offsetOf(impl.Cfg, "pubkey_handle"));
}
