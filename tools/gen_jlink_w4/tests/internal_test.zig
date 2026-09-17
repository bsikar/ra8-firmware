//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the pure half of `gen_jlink_w4` (#858).
//! Every expectation here was established by running the deleted CPython
//! predecessor, not by reading its source: the base-address parser reproduces
//! `int(text, 16)` including its Unicode transform, and the address renderer
//! reproduces `format(value, "08X")` including its spelling of a negative sum.

const std = @import("std");
const implementation = @import("implementation");
const testing = std.testing;

fn parsed(text: []const u8) !implementation.HexLiteral {
    return implementation.pythonHexInt(testing.allocator, text);
}

fn expectValue(text: []const u8, negative: bool, digits: []const u8) !void {
    const literal = try parsed(text);
    defer testing.allocator.free(literal.digits);
    try testing.expectEqual(negative, literal.negative);
    try testing.expectEqualStrings(digits, literal.digits);
}

fn expectRejected(text: []const u8) !void {
    try testing.expectError(implementation.ParseError.InvalidLiteral, parsed(text));
}

fn address(text: []const u8, offset: u64) ![]u8 {
    const literal = try parsed(text);
    defer testing.allocator.free(literal.digits);
    return implementation.formatAddress(testing.allocator, literal, offset);
}

fn expectAddress(text: []const u8, offset: u64, expected: []const u8) !void {
    const text_out = try address(text, offset);
    defer testing.allocator.free(text_out);
    try testing.expectEqualStrings(expected, text_out);
}

test "a plain prefixed literal parses" {
    try expectValue("0x02000000", false, "2000000");
}

test "the prefix is optional" {
    try expectValue("2000000", false, "2000000");
}

test "an upper-case prefix parses" {
    try expectValue("0X20", false, "20");
}

test "surrounding ASCII whitespace is skipped" {
    try expectValue(" 0x20 ", false, "20");
}

test "a trailing newline is whitespace" {
    try expectValue("1\n", false, "1");
}

test "a trailing tab is whitespace" {
    try expectValue("20\t", false, "20");
}

test "a leading plus sign parses" {
    try expectValue("+0x20", false, "20");
}

test "a leading minus sign parses" {
    try expectValue("-1", true, "1");
}

test "digits are upper-cased and leading zeros dropped" {
    try expectValue("0x0001f4", false, "1F4");
}

test "negative zero loses its sign, as int(\"-0\", 16) does" {
    try expectValue("-0", false, "0");
}

test "an unbounded literal keeps every digit" {
    try expectValue("0xffffffffffffffffffff", false, "FFFFFFFFFFFFFFFFFFFF");
}

test "an underscore may follow the prefix" {
    try expectValue("0x_1", false, "1");
    try expectValue("0X_ff", false, "FF");
}

test "an underscore may sit between digits" {
    try expectValue("0x1_0", false, "10");
}

test "a leading underscore is rejected" {
    try expectRejected("_1");
    try expectRejected("-_1");
}

test "a trailing underscore is rejected" {
    try expectRejected("1_");
    try expectRejected("0x0_");
}

test "doubled underscores are rejected" {
    try expectRejected("1__2");
}

test "a bare prefix is rejected" {
    try expectRejected("0x");
    try expectRejected("0x_");
}

test "an empty or blank argument is rejected" {
    try expectRejected("");
    try expectRejected("  ");
}

test "interior whitespace is rejected" {
    try expectRejected("2 0");
    try expectRejected("+ 1");
    try expectRejected("0x 1");
}

test "a doubled sign is rejected" {
    try expectRejected("--1");
    try expectRejected("+-1");
}

test "a non-hex letter is rejected" {
    try expectRejected("zz");
    try expectRejected("0xg");
    try expectRejected("0x1g");
}

test "an octal prefix is rejected while a binary one is read as hex digits" {
    try expectRejected("0o7");
    // int("0b10", 16) is 0xb10: b, 1 and 0 are all hex digits.
    try expectValue("0b10", false, "B10");
}

test "a non-ASCII space is transformed to a space" {
    try expectValue("\u{a0}20", false, "20");
    try expectValue("\u{3000}0x10", false, "10");
    try expectValue("\u{2028}20", false, "20");
    try expectValue("\u{85}20", false, "20");
}

test "U+000B is ASCII whitespace but U+001C and U+001F are not" {
    try expectValue("\u{b}20", false, "20");
    // The transform leaves ASCII alone and C's isspace rejects these two.
    try expectRejected("\u{1c}20");
    try expectRejected("\u{1f}20");
}

test "a non-ASCII decimal digit becomes its ASCII digit" {
    try expectValue("\u{663}", false, "3");
    try expectValue("\u{663}20", false, "320");
    try expectValue("\u{ff11}\u{ff12}", false, "12");
    try expectValue("0x\u{ff11}", false, "1");
}

test "a non-ASCII digit may sit either side of an underscore" {
    try expectValue("\u{663}_\u{663}", false, "33");
    try expectValue("\u{ff11}_\u{ff12}", false, "12");
}

test "a fullwidth letter is not a hex digit" {
    try expectRejected("\u{ff21}\u{ff22}");
    try expectRejected("\u{ff41}1");
    try expectRejected("\u{ff11}x1");
}

test "an undecodable byte cannot parse" {
    try expectRejected("\xff20");
    try expectRejected("0x\x80");
}

test "isNonAsciiSpace covers the table and nothing below it" {
    try testing.expect(implementation.isNonAsciiSpace(0x3000));
    try testing.expect(implementation.isNonAsciiSpace(0x2000));
    try testing.expect(implementation.isNonAsciiSpace(0x200A));
    try testing.expect(!implementation.isNonAsciiSpace(' '));
    try testing.expect(!implementation.isNonAsciiSpace(0x200B));
}

test "nonAsciiDigitValue reads a whole run and stops at its end" {
    try testing.expectEqual(@as(?u4, 0), implementation.nonAsciiDigitValue(0x660));
    try testing.expectEqual(@as(?u4, 9), implementation.nonAsciiDigitValue(0x669));
    try testing.expectEqual(@as(?u4, null), implementation.nonAsciiDigitValue(0x66A));
    try testing.expectEqual(@as(?u4, null), implementation.nonAsciiDigitValue('7'));
}

test "isAsciiSpace is C's isspace, not str.isspace" {
    try testing.expect(implementation.isAsciiSpace(' '));
    try testing.expect(implementation.isAsciiSpace('\t'));
    try testing.expect(implementation.isAsciiSpace(0x0B));
    try testing.expect(implementation.isAsciiSpace(0x0C));
    try testing.expect(!implementation.isAsciiSpace(0x1C));
    try testing.expect(!implementation.isAsciiSpace(0x1F));
}

test "decodeAt reports one code point and its length" {
    const decoded = implementation.decodeAt("\u{3000}x", 0);
    try testing.expectEqual(@as(u21, 0x3000), decoded.code_point);
    try testing.expectEqual(@as(usize, 3), decoded.len);
}

test "decodeAt escapes a stray byte the way surrogateescape does" {
    const decoded = implementation.decodeAt("\xff", 0);
    try testing.expectEqual(@as(u21, 0xDC80 + 0xFF), decoded.code_point);
    try testing.expectEqual(@as(usize, 1), decoded.len);
}

test "decodeAt escapes a truncated sequence" {
    const decoded = implementation.decodeAt("\xe3\x80", 0);
    try testing.expectEqual(@as(u21, 0xDC80 + 0xE3), decoded.code_point);
    try testing.expectEqual(@as(usize, 1), decoded.len);
}

test "transformForInt keeps ASCII and rewrites the rest" {
    const transformed = try implementation.transformForInt(testing.allocator, "a\u{3000}\u{663}\u{2603}");
    defer testing.allocator.free(transformed);
    try testing.expectEqualStrings(&[_]u8{ 'a', ' ', '3', implementation.opaque_byte }, transformed);
}

test "an address is eight upper-case hex characters" {
    try expectAddress("0x0", 0, "00000000");
    try expectAddress("0x02000000", 0, "02000000");
    try expectAddress("0xff", 0, "000000FF");
}

test "the word offset is added to the base" {
    try expectAddress("0x02000000", 4, "02000004");
    try expectAddress("0x0ffffffc", 4, "10000000");
    try expectAddress("0xfffffffc", 8, "100000004"[0..9]);
}

test "an address wider than eight digits is not truncated" {
    try expectAddress("0x100000000", 0, "100000000");
    try expectAddress("0xffffffffffffffffffff", 1, "100000000000000000000");
}

test "a negative base keeps Python's sign-then-pad spelling" {
    try expectAddress("-1", 0, "-0000001");
    try expectAddress("-0x12345678", 0, "-12345678");
}

test "a negative base walks up through zero as the offset grows" {
    try expectAddress("-4", 0, "-0000004");
    try expectAddress("-4", 4, "00000000");
    try expectAddress("-4", 8, "00000004");
    try expectAddress("-1", 4, "00000003");
}

test "a negative base of many digits borrows correctly" {
    try expectAddress("-0x100000000", 1, "-FFFFFFFF");
    try expectAddress("-0x10", 0x20, "00000010");
}

test "formatWord pads and upper-cases a 32-bit word" {
    try testing.expectEqualStrings("00000000", &implementation.formatWord(0));
    try testing.expectEqualStrings("000000FF", &implementation.formatWord(0xFF));
    try testing.expectEqualStrings("DEADBEEF", &implementation.formatWord(0xDEADBEEF));
    try testing.expectEqualStrings("FFFFFFFF", &implementation.formatWord(0xFFFFFFFF));
}

test "padToWord leaves a whole number of words alone" {
    const padded = try implementation.padToWord(testing.allocator, &[_]u8{ 1, 2, 3, 4 });
    defer testing.allocator.free(padded);
    try testing.expectEqualSlices(u8, &[_]u8{ 1, 2, 3, 4 }, padded);
}

test "padToWord fills a short final word with 0xFF" {
    const padded = try implementation.padToWord(testing.allocator, &[_]u8{ 1, 2, 3, 4, 5 });
    defer testing.allocator.free(padded);
    try testing.expectEqualSlices(u8, &[_]u8{ 1, 2, 3, 4, 5, 0xFF, 0xFF, 0xFF }, padded);
}

test "padToWord leaves an empty image empty, since 0 % 4 is falsy" {
    const padded = try implementation.padToWord(testing.allocator, "");
    defer testing.allocator.free(padded);
    try testing.expectEqual(@as(usize, 0), padded.len);
}

test "wordAt reads little-endian and refuses to run off the end" {
    const image = [_]u8{ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    try testing.expectEqual(@as(?u32, 0x04030201), implementation.wordAt(&image, 0));
    try testing.expectEqual(@as(?u32, null), implementation.wordAt(&image, 4));
    try testing.expectEqual(@as(?u32, null), implementation.wordAt(&image, 6));
}

test "entryOf masks the reset handler's Thumb bit" {
    const image = [_]u8{ 0x00, 0x00, 0x01, 0x20, 0x0D, 0x00, 0x00, 0x02 };
    const entry = implementation.entryOf(&image).?;
    try testing.expectEqual(@as(u32, 0x20010000), entry.initial_sp);
    try testing.expectEqual(@as(u32, 0x0200000C), entry.reset_handler);
}

test "entryOf refuses an image with no second word" {
    try testing.expectEqual(@as(?implementation.Entry, null), implementation.entryOf(&[_]u8{}));
    try testing.expectEqual(@as(?implementation.Entry, null), implementation.entryOf(&[_]u8{ 1, 2, 3, 4 }));
}

test "the padded 0xFF tail reaches the reset handler" {
    const padded = try implementation.padToWord(testing.allocator, &[_]u8{ 1, 2, 3, 4, 5 });
    defer testing.allocator.free(padded);
    const entry = implementation.entryOf(padded).?;
    try testing.expectEqual(@as(u32, 0xFFFFFF04), entry.reset_handler);
}

fn renderWith(comptime write: anytype, argument: anytype) ![]u8 {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    errdefer buffer.deinit();
    try write(buffer.writer(), argument);
    return buffer.toOwnedSlice();
}

test "the preamble is the five fixed lines with the device first" {
    const text = try renderWith(implementation.writePreamble, @as([]const u8, "R7KA8D2KF_CPU0"));
    defer testing.allocator.free(text);
    try testing.expectEqualStrings(
        "device R7KA8D2KF_CPU0\nsi SWD\nspeed 1000\nconnect\nhalt\n",
        text,
    );
}

test "the preamble prints an unusual device name verbatim" {
    const text = try renderWith(implementation.writePreamble, @as([]const u8, "My Device -x"));
    defer testing.allocator.free(text);
    try testing.expectEqualStrings(
        "device My Device -x\nsi SWD\nspeed 1000\nconnect\nhalt\n",
        text,
    );
}

test "a word write names its address and its word" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try implementation.writeWordWrite(buffer.writer(), "02000004", 0xDEADBEEF);
    try testing.expectEqualStrings("w4 0x02000004 0xDEADBEEF\n", buffer.items);
}

test "a word write carries a negative address through unchanged" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try implementation.writeWordWrite(buffer.writer(), "-0000001", 0);
    try testing.expectEqualStrings("w4 0x-0000001 0x00000000\n", buffer.items);
}

test "a register write formats both operands as words" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try implementation.writeRegisterWrite(buffer.writer(), implementation.dcrsr_address, implementation.dcrsr_write_pc);
    try testing.expectEqualStrings("w4 0xE000EDF4 0x0001000F\n", buffer.items);
}

test "the injection sequence is DCRDR then DCRSR, four times, then g and q" {
    const text = try renderWith(implementation.writeEntryInjection, implementation.Entry{
        .initial_sp = 0x20010000,
        .reset_handler = 0x0200000C,
    });
    defer testing.allocator.free(text);
    try testing.expectEqualStrings(
        \\w4 0xE000EDF8 0x20010000
        \\w4 0xE000EDF4 0x00010011
        \\w4 0xE000EDF8 0x01000000
        \\w4 0xE000EDF4 0x00010010
        \\w4 0xE000EDF8 0x00000000
        \\w4 0xE000EDF4 0x00010014
        \\w4 0xE000EDF8 0x0200000C
        \\w4 0xE000EDF4 0x0001000F
        \\g
        \\q
        \\
    , text);
}

test "the injected xPSR sets the T bit and Thread mode and nothing else" {
    try testing.expectEqual(@as(u32, 0x01000000), implementation.thread_mode_xpsr);
}

test "the debug register addresses are the ARMv8-M ones" {
    try testing.expectEqual(@as(u32, 0xE000EDF8), implementation.dcrdr_address);
    try testing.expectEqual(@as(u32, 0xE000EDF4), implementation.dcrsr_address);
}

test "the DCRSR selectors set the write bit and the documented indices" {
    try testing.expectEqual(@as(u32, 0x00010011), implementation.dcrsr_write_msp);
    try testing.expectEqual(@as(u32, 0x00010010), implementation.dcrsr_write_xpsr);
    try testing.expectEqual(@as(u32, 0x00010014), implementation.dcrsr_write_cfbp);
    try testing.expectEqual(@as(u32, 0x0001000F), implementation.dcrsr_write_pc);
}

test "the inherited constants keep their values" {
    try testing.expectEqual(@as(u32, 0xFFFFFFFE), implementation.reset_handler_mask);
    try testing.expectEqual(@as(u8, 0xFF), implementation.pad_byte);
    try testing.expectEqual(@as(usize, 8), implementation.min_image_bytes);
    try testing.expectEqualStrings("R7KA8D2KF_CPU0", implementation.default_device);
}

test "isZero recognises the single zero digit only" {
    const zero = try parsed("0x0");
    defer testing.allocator.free(zero.digits);
    try testing.expect(zero.isZero());
    const one = try parsed("0x1");
    defer testing.allocator.free(one.digits);
    try testing.expect(!one.isZero());
}
