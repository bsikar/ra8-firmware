//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure computation behind the J-Link w4 script generator (#858). Nothing here
//! opens a file, reads the environment or looks at argv: this module turns an
//! argument string into the integer CPython's `int(text, 16)` would have
//! produced, turns that integer plus a word offset into the exact text
//! `format(value, "08X")` would have produced, and renders the script lines.
//!
//! The predecessor, scripts/gen/gen_jlink_w4.py, held a base address as a
//! Python `int`, which is unbounded and signed. Both properties are observable
//! through the emitted script: `0xffffffffffffffffffff` is a legal base and
//! prints twenty hex digits, and a negative base prints Python's sign-then-pad
//! spelling, `w4 0x-0000001`. A `u64` would silently wrap the first and reject
//! the second, so the magnitude is kept as a little-endian nibble vector and
//! the arithmetic is done here.

const std = @import("std");
const char_classes = @import("char_classes.zig");

/// Register-injection addresses from the ARMv8-M debug interface (C1.6).
pub const dcrdr_address: u32 = 0xE000EDF8;
pub const dcrsr_address: u32 = 0xE000EDF4;

/// DCRSR selectors: bit 16 is the write bit, the low bits the register index.
pub const dcrsr_write_msp: u32 = 0x00010011;
pub const dcrsr_write_xpsr: u32 = 0x00010010;
pub const dcrsr_write_cfbp: u32 = 0x00010014;
pub const dcrsr_write_pc: u32 = 0x0001000F;

/// xPSR value injected before "g": T bit set, Thread mode.
pub const thread_mode_xpsr: u32 = 0x01000000;

/// The reset handler's Thumb bit is cleared before it reaches the PC.
pub const reset_handler_mask: u32 = 0xFFFFFFFE;

/// Byte the predecessor padded a short final word with.
pub const pad_byte: u8 = 0xFF;

/// Default device, as the predecessor spelled it.
pub const default_device = "R7KA8D2KF_CPU0";

/// Minimum image length: the vector table's first two words must be readable.
pub const min_image_bytes: usize = 8;

/// One decoded code point and the number of bytes it occupied.
pub const Decoded = struct { code_point: u21, len: usize };

/// Decode one UTF-8 sequence, emulating Python's surrogateescape: a byte that
/// cannot start or complete a sequence decodes to U+DC80 + byte, which is
/// neither a space nor a digit and so can only make a parse fail, exactly as
/// an undecodable argument does under CPython.
pub fn decodeAt(text: []const u8, index: usize) Decoded {
    const first = text[index];
    const len = std.unicode.utf8ByteSequenceLength(first) catch
        return .{ .code_point = 0xDC80 + @as(u21, first), .len = 1 };
    if (index + len > text.len) return .{ .code_point = 0xDC80 + @as(u21, first), .len = 1 };
    const code_point = std.unicode.utf8Decode(text[index .. index + len]) catch
        return .{ .code_point = 0xDC80 + @as(u21, first), .len = 1 };
    return .{ .code_point = code_point, .len = len };
}

/// True when `Py_UNICODE_ISSPACE` accepts this non-ASCII code point.
pub fn isNonAsciiSpace(code_point: u21) bool {
    if (code_point < 0x80) return false;
    for (char_classes.non_ascii_space_intervals) |interval| {
        if (code_point >= interval[0] and code_point <= interval[1]) return true;
    }
    return false;
}

/// Decimal value of a non-ASCII digit, as `Py_UNICODE_TODECIMAL` reports it.
pub fn nonAsciiDigitValue(code_point: u21) ?u4 {
    if (code_point < 0x80) return null;
    for (char_classes.non_ascii_digit_run_starts) |start| {
        if (code_point >= start and code_point < start + 10) {
            return @intCast(code_point - start);
        }
    }
    return null;
}

/// True for the six bytes C's `isspace` accepts, which is what CPython's
/// integer parser skips once the transform below has run. Deliberately NOT
/// `str.isspace`: U+001C..U+001F are whitespace to `str.isspace` and are not
/// skipped here, which is why `int("\x1c20", 16)` raises.
pub fn isAsciiSpace(byte: u8) bool {
    return switch (byte) {
        ' ', '\t', '\n', 0x0B, 0x0C, '\r' => true,
        else => false,
    };
}

/// Byte that no parse step accepts, standing in for a non-ASCII code point the
/// transform leaves alone.
pub const opaque_byte: u8 = 0xFF;

/// Reproduce `_PyUnicode_TransformDecimalAndSpaceToASCII`: ASCII survives
/// untouched, a non-ASCII space becomes one space, a non-ASCII decimal digit
/// becomes its ASCII digit, and anything else becomes a byte the parser must
/// reject. The result is therefore pure ASCII and one byte per code point.
pub fn transformForInt(allocator: std.mem.Allocator, text: []const u8) ![]u8 {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    var index: usize = 0;
    while (index < text.len) {
        const decoded = decodeAt(text, index);
        index += decoded.len;
        if (decoded.code_point < 0x80) {
            try out.append(@intCast(decoded.code_point));
        } else if (isNonAsciiSpace(decoded.code_point)) {
            try out.append(' ');
        } else if (nonAsciiDigitValue(decoded.code_point)) |value| {
            try out.append('0' + @as(u8, value));
        } else {
            try out.append(opaque_byte);
        }
    }
    return out.toOwnedSlice();
}

/// What CPython raises for a malformed literal.
pub const ParseError = error{InvalidLiteral};

/// A parsed base-16 literal: its sign and its magnitude as hex digits with no
/// separators, no prefix and no leading zeros. Zero is always non-negative,
/// since `int("-0", 16)` is `0` and prints without a sign.
pub const HexLiteral = struct {
    negative: bool,
    digits: []const u8,

    pub fn isZero(self: HexLiteral) bool {
        return self.digits.len == 1 and self.digits[0] == '0';
    }
};

fn hexValue(byte: u8) ?u4 {
    return switch (byte) {
        '0'...'9' => @intCast(byte - '0'),
        'a'...'f' => @intCast(byte - 'a' + 10),
        'A'...'F' => @intCast(byte - 'A' + 10),
        else => null,
    };
}

/// Parse a transformed string the way `int(text, 16)` does: optional ASCII
/// whitespace, an optional sign, an optional `0x`/`0X` prefix, then hex digits
/// with single underscores between them (one underscore may also follow the
/// prefix), then optional trailing whitespace and nothing else.
pub fn parseHexLiteral(allocator: std.mem.Allocator, transformed: []const u8) !HexLiteral {
    var index: usize = 0;
    while (index < transformed.len and isAsciiSpace(transformed[index])) index += 1;

    var negative = false;
    if (index < transformed.len and (transformed[index] == '+' or transformed[index] == '-')) {
        negative = transformed[index] == '-';
        index += 1;
    }

    var had_prefix = false;
    if (index + 1 < transformed.len and transformed[index] == '0' and
        (transformed[index + 1] == 'x' or transformed[index + 1] == 'X'))
    {
        had_prefix = true;
        index += 2;
    }

    // One underscore may sit between the prefix and the first digit, which is
    // why int("0x_1", 16) is 1 while int("_1", 16) raises.
    if (had_prefix and index < transformed.len and transformed[index] == '_') index += 1;

    var digits = std.ArrayList(u8).init(allocator);
    errdefer digits.deinit();
    if (index >= transformed.len or hexValue(transformed[index]) == null) return ParseError.InvalidLiteral;
    try digits.append(transformed[index]);
    index += 1;
    while (index < transformed.len) {
        const byte = transformed[index];
        if (hexValue(byte) != null) {
            try digits.append(byte);
            index += 1;
            continue;
        }
        if (byte == '_') {
            if (index + 1 >= transformed.len or hexValue(transformed[index + 1]) == null) {
                return ParseError.InvalidLiteral;
            }
            try digits.append(transformed[index + 1]);
            index += 2;
            continue;
        }
        break;
    }

    while (index < transformed.len and isAsciiSpace(transformed[index])) index += 1;
    if (index != transformed.len) return ParseError.InvalidLiteral;

    var first_significant: usize = 0;
    while (first_significant + 1 < digits.items.len and digits.items[first_significant] == '0') {
        first_significant += 1;
    }
    const significant = digits.items[first_significant..];
    const normalized = try allocator.alloc(u8, significant.len);
    for (significant, 0..) |byte, offset| normalized[offset] = std.ascii.toUpper(byte);
    digits.deinit();

    const zero = normalized.len == 1 and normalized[0] == '0';
    return .{ .negative = negative and !zero, .digits = normalized };
}

/// Parse an argument exactly as `int(argument, 16)` would.
pub fn pythonHexInt(allocator: std.mem.Allocator, argument: []const u8) !HexLiteral {
    const transformed = try transformForInt(allocator, argument);
    defer allocator.free(transformed);
    return parseHexLiteral(allocator, transformed);
}

/// Magnitude as little-endian nibbles, most significant nibble last and no
/// leading zero nibbles except for the single nibble of zero itself.
fn nibblesFromDigits(allocator: std.mem.Allocator, digits: []const u8) ![]u4 {
    const nibbles = try allocator.alloc(u4, digits.len);
    for (digits, 0..) |digit, offset| {
        nibbles[digits.len - 1 - offset] = hexValue(digit).?;
    }
    return nibbles;
}

fn addOffset(allocator: std.mem.Allocator, nibbles: []const u4, offset: u64) ![]u4 {
    var out = std.ArrayList(u4).init(allocator);
    errdefer out.deinit();
    var carry: u64 = offset;
    for (nibbles) |nibble| {
        const sum = @as(u64, nibble) + (carry & 0xF);
        try out.append(@intCast(sum & 0xF));
        carry = (carry >> 4) + (sum >> 4);
    }
    while (carry != 0) {
        try out.append(@intCast(carry & 0xF));
        carry >>= 4;
    }
    return out.toOwnedSlice();
}

fn compareWithOffset(nibbles: []const u4, offset: u64) std.math.Order {
    var scratch: [16]u4 = undefined;
    var length: usize = 0;
    var remaining = offset;
    while (remaining != 0 and length < scratch.len) {
        scratch[length] = @intCast(remaining & 0xF);
        remaining >>= 4;
        length += 1;
    }
    var significant = nibbles.len;
    while (significant > 0 and nibbles[significant - 1] == 0) significant -= 1;
    if (significant != length) return if (significant > length) .gt else .lt;
    var index = significant;
    while (index > 0) {
        index -= 1;
        if (nibbles[index] != scratch[index]) {
            return if (nibbles[index] > scratch[index]) .gt else .lt;
        }
    }
    return .eq;
}

fn subtractSmaller(allocator: std.mem.Allocator, nibbles: []const u4, offset: u64) ![]u4 {
    var out = std.ArrayList(u4).init(allocator);
    errdefer out.deinit();
    var borrow: i64 = 0;
    var remaining = offset;
    for (nibbles) |nibble| {
        var value: i64 = @as(i64, nibble) - @as(i64, @intCast(remaining & 0xF)) - borrow;
        remaining >>= 4;
        if (value < 0) {
            value += 16;
            borrow = 1;
        } else borrow = 0;
        try out.append(@intCast(value));
    }
    return out.toOwnedSlice();
}

fn subtractFromOffset(allocator: std.mem.Allocator, offset: u64, nibbles: []const u4) ![]u4 {
    var out = std.ArrayList(u4).init(allocator);
    errdefer out.deinit();
    var borrow: i64 = 0;
    var remaining = offset;
    var index: usize = 0;
    while (remaining != 0 or index < nibbles.len) {
        const minuend: i64 = @intCast(remaining & 0xF);
        const subtrahend: i64 = if (index < nibbles.len) @as(i64, nibbles[index]) else 0;
        var value: i64 = minuend - subtrahend - borrow;
        if (value < 0) {
            value += 16;
            borrow = 1;
        } else borrow = 0;
        try out.append(@intCast(value));
        remaining >>= 4;
        index += 1;
    }
    return out.toOwnedSlice();
}

fn renderNibbles(allocator: std.mem.Allocator, nibbles: []const u4, negative: bool, width: usize) ![]u8 {
    var significant = nibbles.len;
    while (significant > 1 and nibbles[significant - 1] == 0) significant -= 1;
    const is_zero = significant == 1 and nibbles[0] == 0;
    const sign: usize = if (negative and !is_zero) 1 else 0;
    const body = if (significant + sign >= width) significant else width - sign;
    var out = try allocator.alloc(u8, body + sign);
    if (sign == 1) out[0] = '-';
    var position: usize = sign;
    var zeros = body - significant;
    while (zeros > 0) : (zeros -= 1) {
        out[position] = '0';
        position += 1;
    }
    var index = significant;
    while (index > 0) {
        index -= 1;
        out[position] = "0123456789ABCDEF"[nibbles[index]];
        position += 1;
    }
    return out;
}

/// Text `format(base + offset, "08X")` produces: uppercase hex, at least eight
/// characters, and for a negative sum Python's sign-then-zeros spelling, so
/// `-1` prints as `-0000001` and not `-00000001`.
pub fn formatAddress(
    allocator: std.mem.Allocator,
    base: HexLiteral,
    offset: u64,
) ![]u8 {
    const magnitude = try nibblesFromDigits(allocator, base.digits);
    defer allocator.free(magnitude);
    if (!base.negative) {
        const sum = try addOffset(allocator, magnitude, offset);
        defer allocator.free(sum);
        return renderNibbles(allocator, sum, false, 8);
    }
    switch (compareWithOffset(magnitude, offset)) {
        .gt => {
            const difference = try subtractSmaller(allocator, magnitude, offset);
            defer allocator.free(difference);
            return renderNibbles(allocator, difference, true, 8);
        },
        .eq => {
            const zero = [_]u4{0};
            return renderNibbles(allocator, &zero, false, 8);
        },
        .lt => {
            const difference = try subtractFromOffset(allocator, offset, magnitude);
            defer allocator.free(difference);
            return renderNibbles(allocator, difference, false, 8);
        },
    }
}

/// Text `format(word, "08X")` produces for a 32-bit word.
pub fn formatWord(word: u32) [8]u8 {
    var out: [8]u8 = undefined;
    var index: usize = 8;
    var remaining = word;
    while (index > 0) {
        index -= 1;
        out[index] = "0123456789ABCDEF"[@as(u4, @intCast(remaining & 0xF))];
        remaining >>= 4;
    }
    return out;
}

/// Pad an image with 0xFF until its length is a whole number of words, which
/// is the `while len(data) % 4: data += b"\xff"` the predecessor ran.
pub fn padToWord(allocator: std.mem.Allocator, image: []const u8) ![]u8 {
    const padded_length = image.len + (std.mem.alignForward(usize, image.len, 4) - image.len);
    const padded = try allocator.alloc(u8, padded_length);
    @memcpy(padded[0..image.len], image);
    @memset(padded[image.len..], pad_byte);
    return padded;
}

/// Little-endian word at a byte offset, or null when the buffer is too short,
/// which is where `struct.unpack_from` raised.
pub fn wordAt(image: []const u8, offset: usize) ?u32 {
    if (offset + 4 > image.len) return null;
    return std.mem.readInt(u32, image[offset..][0..4], .little);
}

/// The two vector-table entries the script injects into the core registers.
pub const Entry = struct { initial_sp: u32, reset_handler: u32 };

/// Read the vector table's first two words, masking the reset handler's Thumb
/// bit off exactly as the predecessor's `& 0xFFFFFFFE` did.
pub fn entryOf(image: []const u8) ?Entry {
    const initial_sp = wordAt(image, 0) orelse return null;
    const reset_handler = wordAt(image, 4) orelse return null;
    return .{ .initial_sp = initial_sp, .reset_handler = reset_handler & reset_handler_mask };
}

/// The five fixed lines that open every script.
pub fn writePreamble(writer: anytype, device: []const u8) !void {
    try writer.print("device {s}\nsi SWD\nspeed 1000\nconnect\nhalt\n", .{device});
}

/// One `w4 <address> <word>` line.
pub fn writeWordWrite(writer: anytype, address: []const u8, word: u32) !void {
    try writer.print("w4 0x{s} 0x{s}\n", .{ address, formatWord(word) });
}

/// One `w4` line against a fixed 32-bit debug register address.
pub fn writeRegisterWrite(writer: anytype, address: u32, word: u32) !void {
    try writer.print("w4 0x{s} 0x{s}\n", .{ formatWord(address), formatWord(word) });
}

/// The DCRSR/DCRDR sequence that loads MSP, xPSR, CFBP and PC, then the "g"
/// and "q" that start the firmware without ever issuing SYSRESETREQ.
pub fn writeEntryInjection(writer: anytype, entry: Entry) !void {
    try writeRegisterWrite(writer, dcrdr_address, entry.initial_sp);
    try writeRegisterWrite(writer, dcrsr_address, dcrsr_write_msp);
    try writeRegisterWrite(writer, dcrdr_address, thread_mode_xpsr);
    try writeRegisterWrite(writer, dcrsr_address, dcrsr_write_xpsr);
    try writeRegisterWrite(writer, dcrdr_address, 0);
    try writeRegisterWrite(writer, dcrsr_address, dcrsr_write_cfbp);
    try writeRegisterWrite(writer, dcrdr_address, entry.reset_handler);
    try writeRegisterWrite(writer, dcrsr_address, dcrsr_write_pc);
    try writer.writeAll("g\nq\n");
}
