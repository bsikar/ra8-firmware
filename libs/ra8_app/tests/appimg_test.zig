//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Format tests for the `.ra8app` container: every refusal the parser can
//! reach, the two spans, format v1 and v2 with icon metadata, and the capability comparison.

const std = @import("std");
const appimg = @import("appimg");

comptime {
    _ = @import("app_api_fake.zig");
}

const header_bytes = @sizeOf(appimg.Header);

/// A header that parses, as the base every negative case perturbs.
fn goodHeader() appimg.Header {
    var h = std.mem.zeroes(appimg.Header);
    h.magic = appimg.Format.magic;
    h.format_version = appimg.Format.version;
    h.entry_offset = 0;
    h.code_size = 256;
    h.data_size = 64;
    h.stack_size = appimg.Format.stack_min;
    h.min_api_version = appimg.Format.api_version_current;
    h.capabilities = appimg.Capability.display;
    @memcpy(h.app_id[0..11], "com.ex.rdr\x00");
    @memcpy(h.display_name[0..7], "Reader\x00");
    h.icon_offset = 0;
    h.icon_length = 0;
    @memset(&h.signature, 0xAB);
    return h;
}

/// Serialise a header followed by the payload it declares.
fn imageOf(allocator: std.mem.Allocator, h: appimg.Header) ![]u8 {
    const hdr_size = h.headerSize();
    const buf = try allocator.alloc(u8, hdr_size + h.payloadLen());
    @memset(buf, 0);
    if (h.format_version == appimg.Format.version_v1) {
        @memcpy(buf[0..appimg.Header.signature_offset_v1], std.mem.asBytes(&h)[0..appimg.Header.signature_offset_v1]);
        @memcpy(buf[appimg.Header.signature_offset_v1..hdr_size], &h.signature);
    } else {
        @memcpy(buf[0..hdr_size], std.mem.asBytes(&h));
    }
    return buf;
}

test "the on-disk header has the wire layout the format pins" {
    try std.testing.expectEqual(@as(usize, 160), appimg.Format.header_bytes_v1);
    try std.testing.expectEqual(@as(usize, 168), appimg.Format.header_bytes_v2);
    try std.testing.expectEqual(@as(usize, 168), header_bytes);
    try std.testing.expectEqual(@as(usize, 96), appimg.Header.signature_offset_v1);
    try std.testing.expectEqual(@as(usize, 104), appimg.Header.signature_offset_v2);
}

test "a well-formed image parses and round-trips its fields" {
    const h = goodHeader();
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    const parsed = try appimg.parse(image);
    try std.testing.expectEqual(h.code_size, parsed.code_size);
    try std.testing.expectEqual(h.data_size, parsed.data_size);
    try std.testing.expectEqual(appimg.Capability.display, parsed.capabilities);
    try std.testing.expectEqualStrings("com.ex.rdr", std.mem.sliceTo(&parsed.app_id, 0));
}

test "format v1 legacy header parses correctly into in-memory Header" {
    var v1 = goodHeader();
    v1.format_version = appimg.Format.version_v1;
    v1.icon_offset = 0;
    v1.icon_length = 0;

    const image = try imageOf(std.testing.allocator, v1);
    defer std.testing.allocator.free(image);

    try std.testing.expectEqual(@as(usize, 160 + 256 + 64), image.len);
    const parsed = try appimg.parse(image);
    try std.testing.expectEqual(@as(u32, 1), parsed.format_version);
    try std.testing.expectEqual(@as(u32, 0), parsed.icon_offset);
    try std.testing.expectEqual(@as(u32, 0), parsed.icon_length);
    try std.testing.expectEqual(v1.code_size, parsed.code_size);
    try std.testing.expectEqual(v1.data_size, parsed.data_size);
    try std.testing.expectEqualSlices(u8, &v1.signature, &parsed.signature);

    const span = try appimg.signedSpan(parsed, image.len);
    try std.testing.expectEqual(@as(u32, 96), span.length);

    const payload = try appimg.payloadSpan(parsed, image.len);
    try std.testing.expectEqual(@as(u32, 160), payload.offset);
    try std.testing.expectEqual(@as(u32, 320), payload.length);
}

test "format v2 with icon parses and reports valid icon span" {
    var v2 = goodHeader();
    v2.format_version = appimg.Format.version_v2;
    v2.icon_offset = v2.code_size + v2.data_size; // immediately follows code + data
    v2.icon_length = appimg.Format.icon_bytes;

    const image = try imageOf(std.testing.allocator, v2);
    defer std.testing.allocator.free(image);

    const parsed = try appimg.parse(image);
    try std.testing.expectEqual(@as(u32, 2), parsed.format_version);
    try std.testing.expectEqual(@as(u32, 320), parsed.icon_offset);
    try std.testing.expectEqual(@as(u32, 64), parsed.icon_length);

    const span = try appimg.iconSpan(parsed, image.len);
    try std.testing.expectEqual(@as(u32, 168 + 320), span.offset);
    try std.testing.expectEqual(@as(u32, 64), span.length);

    const slice = (try appimg.iconSlice(parsed, image)).?;
    try std.testing.expectEqual(@as(usize, 64), slice.len);
}

test "an icon length other than 64 when non-zero is out of range" {
    var v2 = goodHeader();
    v2.icon_offset = v2.code_size + v2.data_size;
    v2.icon_length = 63;
    const image = try imageOf(std.testing.allocator, v2);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.OutOfRange, appimg.parse(image));

    var v2_big = goodHeader();
    v2_big.icon_offset = v2_big.code_size + v2_big.data_size;
    v2_big.icon_length = 65;
    const image2 = try imageOf(std.testing.allocator, v2_big);
    defer std.testing.allocator.free(image2);
    try std.testing.expectError(appimg.Error.OutOfRange, appimg.parse(image2));
}

test "an icon overlapping code or data is out of range" {
    var v2 = goodHeader();
    v2.icon_offset = (v2.code_size + v2.data_size) - 1; // overlaps data!
    v2.icon_length = appimg.Format.icon_bytes;
    const image = try imageOf(std.testing.allocator, v2);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.OutOfRange, appimg.parse(image));
}

test "a non-zero icon offset when icon length is zero is a validation refusal" {
    var v2 = goodHeader();
    v2.icon_offset = 16;
    v2.icon_length = 0;
    const image = try imageOf(std.testing.allocator, v2);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.Validation, appimg.parse(image));
}

test "an image shorter than the header is refused before anything is read" {
    const short: [160 - 1]u8 = @splat(0);
    try std.testing.expectError(appimg.Error.ShortImage, appimg.parse(&short));
}

test "the wrong magic is a validation refusal" {
    var h = goodHeader();
    h.magic = 0xDEADBEEF;
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.Validation, appimg.parse(image));
}

test "a format revision this build does not read is a validation refusal" {
    var h = goodHeader();
    h.format_version = appimg.Format.version + 1;
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.Validation, appimg.parse(image));
}

test "an undefined capability bit is refused rather than ignored" {
    var h = goodHeader();
    h.capabilities = appimg.Capability.known | 0x10;
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.Validation, appimg.parse(image));
}

test "a text field with no NUL inside its width is refused" {
    var h = goodHeader();
    @memset(&h.app_id, 'x');
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.Validation, appimg.parse(image));

    var g = goodHeader();
    @memset(&g.display_name, 'y');
    const image2 = try imageOf(std.testing.allocator, g);
    defer std.testing.allocator.free(image2);
    try std.testing.expectError(appimg.Error.Validation, appimg.parse(image2));
}

test "an empty app_id is refused" {
    var h = goodHeader();
    @memset(&h.app_id, 0);
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.Validation, appimg.parse(image));
}

test "an app newer than this firmware is unsupported, not malformed" {
    var h = goodHeader();
    h.min_api_version = appimg.Format.api_version_current + 1;
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.Unsupported, appimg.parse(image));
}

test "a zero or over-cap code size is out of range" {
    var zero = goodHeader();
    zero.code_size = 0;
    zero.entry_offset = 0;
    const a = try imageOf(std.testing.allocator, zero);
    defer std.testing.allocator.free(a);
    try std.testing.expectError(appimg.Error.OutOfRange, appimg.parse(a));

    var over = goodHeader();
    over.code_size = appimg.Format.segment_max + 1;
    over.data_size = 0;
    const b = try std.testing.allocator.alloc(u8, header_bytes);
    defer std.testing.allocator.free(b);
    @memcpy(b, std.mem.asBytes(&over));
    try std.testing.expectError(appimg.Error.OutOfRange, appimg.parse(b));
}

test "a stack below the floor or above the cap is out of range" {
    var low = goodHeader();
    low.stack_size = appimg.Format.stack_min - 1;
    const a = try imageOf(std.testing.allocator, low);
    defer std.testing.allocator.free(a);
    try std.testing.expectError(appimg.Error.OutOfRange, appimg.parse(a));

    var high = goodHeader();
    high.stack_size = appimg.Format.segment_max + 1;
    const b = try imageOf(std.testing.allocator, high);
    defer std.testing.allocator.free(b);
    try std.testing.expectError(appimg.Error.OutOfRange, appimg.parse(b));
}

test "an entry offset outside the code area is out of range" {
    var h = goodHeader();
    h.entry_offset = h.code_size;
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.OutOfRange, appimg.parse(image));
}

test "a file too short for the payload it declares is refused on size" {
    const h = goodHeader();
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(appimg.Error.ShortImage, appimg.parse(image[0 .. image.len - 1]));
}

test "the payload length cannot overflow the size check" {
    var h = goodHeader();
    h.code_size = appimg.Format.segment_max;
    h.data_size = appimg.Format.segment_max;
    h.entry_offset = 0;
    var buf: [header_bytes]u8 = undefined;
    @memcpy(&buf, std.mem.asBytes(&h));
    try std.testing.expectError(appimg.Error.ShortImage, appimg.parse(&buf));
}

test "the signed span ends where the signature field begins" {
    const h = goodHeader();
    const span = try appimg.signedSpan(h, header_bytes);
    try std.testing.expectEqual(@as(u32, 0), span.offset);
    try std.testing.expectEqual(@as(u32, 104), span.length);
    try std.testing.expectError(appimg.Error.ShortImage, appimg.signedSpan(h, header_bytes - 1));

    var v1 = h;
    v1.format_version = appimg.Format.version_v1;
    const span_v1 = try appimg.signedSpan(v1, appimg.Format.header_bytes_v1);
    try std.testing.expectEqual(@as(u32, 0), span_v1.offset);
    try std.testing.expectEqual(@as(u32, 96), span_v1.length);
}

test "the payload span starts after the header and covers code plus data" {
    const h = goodHeader();
    const span = try appimg.payloadSpan(h, h.imageLen());
    try std.testing.expectEqual(@as(u32, header_bytes), span.offset);
    try std.testing.expectEqual(h.code_size + h.data_size, span.length);
    try std.testing.expectError(
        appimg.Error.ShortImage,
        appimg.payloadSpan(h, h.imageLen() - 1),
    );
}

test "a grant that covers the manifest permits the load" {
    const h = goodHeader();
    try std.testing.expectEqual(
        @as(?appimg.CapabilityRefusal, null),
        appimg.capabilitiesPermitted(h, appimg.Capability.known),
    );
    try std.testing.expectEqual(
        @as(?appimg.CapabilityRefusal, null),
        appimg.capabilitiesPermitted(h, appimg.Capability.display),
    );
}

test "a withheld capability refuses, and an app declaring nothing never does" {
    var h = goodHeader();
    h.capabilities = appimg.Capability.display | appimg.Capability.network;
    try std.testing.expectEqual(
        appimg.CapabilityRefusal.withheld,
        appimg.capabilitiesPermitted(h, appimg.Capability.display).?,
    );

    h.capabilities = appimg.Capability.none;
    try std.testing.expectEqual(
        @as(?appimg.CapabilityRefusal, null),
        appimg.capabilitiesPermitted(h, appimg.Capability.none),
    );
}

test "a grant carrying an undefined bit is malformed, not merely insufficient" {
    const h = goodHeader();
    try std.testing.expectEqual(
        appimg.CapabilityRefusal.malformed_grant,
        appimg.capabilitiesPermitted(h, appimg.Capability.known | 0x80).?,
    );
}
