//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the log backend's pure halves: decimal formatting, line framing
//! and the error-name table. The ITM transport is target MMIO and is not
//! exercised here; the C never had host coverage of it either.

const std = @import("std");
const format = @import("log_format");
const line = @import("log_line");
const err_names = @import("log_err_names");

// ---- format -------------------------------------------------------------

test "unsigned formats zero as a single digit" {
    var buf: [format.limits.u32_digits]u8 = undefined;
    try std.testing.expectEqualStrings("0", format.unsigned(&buf, 0));
}

test "unsigned formats the widest value" {
    var buf: [format.limits.u32_digits]u8 = undefined;
    try std.testing.expectEqualStrings("4294967295", format.unsigned(&buf, 0xFFFF_FFFF));
}

test "unsigned keeps digits in most-significant-first order" {
    var buf: [format.limits.u32_digits]u8 = undefined;
    try std.testing.expectEqualStrings("1024", format.unsigned(&buf, 1024));
}

test "signed formats a positive value without a sign" {
    var buf: [format.limits.i32_chars]u8 = undefined;
    try std.testing.expectEqualStrings("7", format.signed(&buf, 7));
}

test "signed formats zero" {
    var buf: [format.limits.i32_chars]u8 = undefined;
    try std.testing.expectEqualStrings("0", format.signed(&buf, 0));
}

test "signed formats a negative value" {
    var buf: [format.limits.i32_chars]u8 = undefined;
    try std.testing.expectEqualStrings("-42", format.signed(&buf, -42));
}

test "signed formats the most negative value, which cannot be negated in place" {
    var buf: [format.limits.i32_chars]u8 = undefined;
    try std.testing.expectEqualStrings("-2147483648", format.signed(&buf, std.math.minInt(i32)));
}

test "i32_chars is wide enough for the longest signed rendering" {
    var buf: [format.limits.i32_chars]u8 = undefined;
    try std.testing.expectEqual(
        format.limits.i32_chars,
        format.signed(&buf, std.math.minInt(i32)).len,
    );
}

// ---- line framing -------------------------------------------------------

var captured: [256]u8 = undefined;
var captured_len: usize = 0;

fn capture(byte: u8) void {
    captured[captured_len] = byte;
    captured_len += 1;
}

fn captureReset() void {
    captured_len = 0;
}

fn captured_line() []const u8 {
    return captured[0..captured_len];
}

test "plain frames tag, level and message" {
    captureReset();
    line.plain(capture, "INFO", "ra8_log", "ready");
    try std.testing.expectEqualStrings("[ra8_log] INFO: ready\r\n", captured_line());
}

test "withUnsigned appends the value after an equals sign" {
    captureReset();
    line.withUnsigned(capture, "ERROR", "ra8_dfu", "bad state", 17);
    try std.testing.expectEqualStrings("[ra8_dfu] ERROR: bad state=17\r\n", captured_line());
}

test "withSigned carries the sign into the line" {
    captureReset();
    line.withSigned(capture, "DEBUG", "ra8_time", "drift", -3);
    try std.testing.expectEqualStrings("[ra8_time] DEBUG: drift=-3\r\n", captured_line());
}

test "an empty tag and message still frame" {
    captureReset();
    line.plain(capture, "WARN", "", "");
    try std.testing.expectEqualStrings("[] WARN: \r\n", captured_line());
}

// ---- error names --------------------------------------------------------

test "lookup finds ok at zero" {
    try std.testing.expectEqualStrings("ok", err_names.lookup(0));
}

test "lookup finds a code from each ra8_err.h group" {
    try std.testing.expectEqualStrings("invalid_arg", err_names.lookup(0x103));
    try std.testing.expectEqualStrings("out_of_range", err_names.lookup(0x208));
    try std.testing.expectEqualStrings("rtos_mutex", err_names.lookup(0x304));
    try std.testing.expectEqualStrings("crc_mismatch", err_names.lookup(0x405));
    try std.testing.expectEqualStrings("null_ptr", err_names.lookup(0x504));
}

test "lookup finds the last row" {
    try std.testing.expectEqualStrings("decomp_iterations", err_names.lookup(0x509));
}

test "an unnamed code reports unknown" {
    try std.testing.expectEqualStrings("unknown", err_names.lookup(0x7FFF));
}

test "every name is NUL-terminated so the C caller can read it" {
    for (err_names.table) |entry| {
        try std.testing.expectEqual(@as(u8, 0), entry.name.ptr[entry.name.len]);
    }
}

test "no code appears twice, which would make the first row shadow the second" {
    for (err_names.table, 0..) |entry, index| {
        for (err_names.table[index + 1 ..]) |later| {
            try std.testing.expect(entry.code != later.code);
        }
    }
}
