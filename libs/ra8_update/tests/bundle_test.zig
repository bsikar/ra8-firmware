//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the `"RA8U"` N-image signed bundle parser and validator.

const std = @import("std");
const b = @import("bundle");

fn writeU16(buf: []u8, off: usize, v: u16) void {
    std.mem.writeInt(u16, buf[off..][0..2], v, .little);
}

fn writeU32(buf: []u8, off: usize, v: u32) void {
    std.mem.writeInt(u32, buf[off..][0..4], v, .little);
}

fn makeValidContainer(
    buf: []u8,
    image_count: u16,
    bundle_version: u32,
    total_bytes: u32,
) void {
    @memset(buf, 0);
    writeU32(buf, 0, b.magic);
    writeU16(buf, 4, b.format_version);
    writeU16(buf, 6, b.header_bytes);
    writeU32(buf, 8, bundle_version);
    writeU16(buf, 12, image_count);
    writeU16(buf, 14, 0);
    writeU32(buf, 16, total_bytes);
    writeU32(buf, 20, b.expectedPayloadsOffset(image_count));
    writeU32(buf, 24, b.sig_slot_bytes);
    writeU32(buf, 28, 0);
}

fn writeDesc(
    buf: []u8,
    idx: usize,
    target: b.Target,
    fmt: b.PayloadFmt,
    proto: u32,
    offset: u32,
    size: u32,
) void {
    const base = @as(usize, b.header_bytes) + idx * b.desc_bytes;
    writeU32(buf, base + 0, @backingInt(target));
    writeU16(buf, base + 4, @backingInt(fmt));
    writeU16(buf, base + 6, 0);
    writeU32(buf, base + 8, 5);
    writeU32(buf, base + 12, proto);
    writeU32(buf, base + 16, offset);
    writeU32(buf, base + 20, size);
    writeU32(buf, base + 24, 0);
    writeU32(buf, base + 28, 0);
}

test "single-image RA8 bundle header and descriptor validate cleanly" {
    var buf: [512]u8 = undefined;
    const pay_off = b.expectedPayloadsOffset(1);
    makeValidContainer(&buf, 1, 7, pay_off + 64);
    writeDesc(&buf, 0, .ra8_app, .rot1_img, 0, pay_off, 64);

    const hdr = try b.parseHeader(&buf, 512);
    var descs: [b.max_images]b.ImageDesc = undefined;
    try b.parseAndValidateDescs(&hdr, buf[b.header_bytes..b.sigOffset(&hdr)], &descs);
    const ra8 = b.findTarget(descs[0..1], .ra8_app).?;
    try std.testing.expectEqual(pay_off, ra8.offset);
    try std.testing.expectEqual(@as(u32, 64), ra8.size);
    try std.testing.expectEqual(@as(?b.ImageDesc, null), b.findTarget(descs[0..1], .esp32_c6));
}

test "multi-image RA8 + C6 bundle enforces bounds, alignment, and non-overlap" {
    var buf: [512]u8 = undefined;
    const pay_off = b.expectedPayloadsOffset(2);
    makeValidContainer(&buf, 2, 12, pay_off + 96);
    writeDesc(&buf, 0, .ra8_app, .rot1_img, 3, pay_off, 48); // ends at pay_off+48 -> next 32B aligned is pay_off+64
    writeDesc(&buf, 1, .esp32_c6, .c6_img, 3, pay_off + 64, 32);

    const hdr = try b.parseHeader(&buf, 512);
    var descs: [b.max_images]b.ImageDesc = undefined;
    try b.parseAndValidateDescs(&hdr, buf[b.header_bytes..b.sigOffset(&hdr)], &descs);
    try std.testing.expect(b.compatProtoAllowed(&descs[1], 3));
    try std.testing.expect(!b.compatProtoAllowed(&descs[1], 4));

    // Overlapping second descriptor into padding/payload of first must fail.
    writeDesc(&buf, 1, .esp32_c6, .c6_img, 3, pay_off + 32, 32);
    try std.testing.expectError(
        error.InvalidArg,
        b.parseAndValidateDescs(&hdr, buf[b.header_bytes..b.sigOffset(&hdr)], &descs),
    );
}

test "malformed headers, bad alignment, duplicate targets, and overflow reject" {
    var buf: [512]u8 = undefined;
    const pay_off = b.expectedPayloadsOffset(1);

    makeValidContainer(&buf, 1, 1, pay_off + 32);
    writeU32(&buf, 0, 0xDEAD_BEEF); // bad magic
    try std.testing.expectError(error.InvalidArg, b.parseHeader(&buf, 512));

    makeValidContainer(&buf, 0, 1, pay_off + 32); // zero images
    try std.testing.expectError(error.InvalidArg, b.parseHeader(&buf, 512));

    makeValidContainer(&buf, 1, 1, pay_off + 32);
    writeU32(&buf, 20, pay_off + 32); // wrong payloads_offset
    try std.testing.expectError(error.InvalidSize, b.parseHeader(&buf, 512));

    // Misaligned payload offset.
    makeValidContainer(&buf, 1, 1, pay_off + 64);
    const hdr = try b.parseHeader(&buf, 512);
    var descs: [b.max_images]b.ImageDesc = undefined;
    writeDesc(&buf, 0, .ra8_app, .raw, 0, pay_off + 4, 32);
    try std.testing.expectError(
        error.InvalidArg,
        b.parseAndValidateDescs(&hdr, buf[b.header_bytes..b.sigOffset(&hdr)], &descs),
    );

    // Payload exceeding total_bytes.
    writeDesc(&buf, 0, .ra8_app, .raw, 0, pay_off, 128);
    try std.testing.expectError(
        error.InvalidSize,
        b.parseAndValidateDescs(&hdr, buf[b.header_bytes..b.sigOffset(&hdr)], &descs),
    );
}

test "duplicate targets and unknown formats are refused" {
    var buf: [512]u8 = undefined;
    const pay_off = b.expectedPayloadsOffset(2);
    makeValidContainer(&buf, 2, 1, pay_off + 64);
    const hdr = try b.parseHeader(&buf, 512);
    var descs: [b.max_images]b.ImageDesc = undefined;

    writeDesc(&buf, 0, .ra8_app, .raw, 0, pay_off, 32);
    writeDesc(&buf, 1, .ra8_app, .raw, 0, pay_off + 32, 32);
    try std.testing.expectError(
        error.InvalidArg,
        b.parseAndValidateDescs(&hdr, buf[b.header_bytes..b.sigOffset(&hdr)], &descs),
    );

    const base2 = @as(usize, b.header_bytes) + b.desc_bytes;
    writeDesc(&buf, 1, .esp32_c6, .c6_img, 0, pay_off + 32, 32);
    writeU16(&buf, base2 + 4, 99); // invalid format
    try std.testing.expectError(
        error.InvalidArg,
        b.parseAndValidateDescs(&hdr, buf[b.header_bytes..b.sigOffset(&hdr)], &descs),
    );
}

test "empty and short raw headers fail safely" {
    var short_buf: [16]u8 = @splat(0);
    try std.testing.expectError(error.InvalidSize, b.parseHeader(&short_buf, 512));
}
