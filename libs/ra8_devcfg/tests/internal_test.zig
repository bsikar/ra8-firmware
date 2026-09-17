//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the hardware-agnostic record core: the CRC, the
//! explicit little-endian codec, the four validation gates and the stale-slot
//! selection rule. No store seam is involved, so every case is a pure call.

const std = @import("std");
const implementation = @import("implementation");

fn sampleRecord(vcom: u16, flags: u32) implementation.Record {
    var rec: implementation.Record = .{};
    @memcpy(rec.body.serial[0..7], "SN-0001");
    @memcpy(rec.body.panel_serial[0..8], "PANEL-01");
    @memcpy(rec.body.panel_lut_id[0..4], "M641");
    for (&rec.body.touch_cal, 0..) |*byte, i| byte.* = @intCast(i);
    rec.body.mfg_date = 20260723;
    rec.body.device_key_id = 0xDEADBEEF;
    rec.body.hw_rev = 2;
    rec.body.fixture_id = 7;
    rec.body.panel_vcom_mv = vcom;
    rec.flags = flags;
    return rec;
}

test "crc32 matches the IEEE 802.3 check vector" {
    try std.testing.expectEqual(@as(u32, 0xCBF43926), implementation.crc32("123456789"));
}

test "crc32 of an empty span is zero" {
    try std.testing.expectEqual(@as(u32, 0), implementation.crc32(""));
}

test "crc32 changes on a single-bit flip" {
    var body: [16]u8 = @splat(0x5A);
    const before = implementation.crc32(&body);
    body[7] ^= 0x01;
    try std.testing.expect(before != implementation.crc32(&body));
}

test "crc32 matches the std implementation over a long span" {
    var span: [257]u8 = undefined;
    for (&span, 0..) |*byte, i| byte.* = @truncate(i *% 31);
    try std.testing.expectEqual(std.hash.Crc32.hash(&span), implementation.crc32(&span));
}

test "le16 round-trips including the poison values" {
    var buf: [2]u8 = @splat(0);
    for ([_]u16{ 0, 1, 0x1234, 0x00FF, 0xFF00, 0xFFFF }) |value| {
        implementation.packLe16(&buf, value);
        try std.testing.expectEqual(value, implementation.unpackLe16(&buf));
    }
}

test "le16 writes the low byte first" {
    var buf: [2]u8 = @splat(0);
    implementation.packLe16(&buf, 0xBEEF);
    try std.testing.expectEqual(@as(u8, 0xEF), buf[0]);
    try std.testing.expectEqual(@as(u8, 0xBE), buf[1]);
}

test "le32 round-trips including the bounds" {
    var buf: [4]u8 = @splat(0);
    for ([_]u32{ 0, 1, 0xDEADBEEF, 0x000000FF, 0xFF000000, 0xFFFFFFFF }) |value| {
        implementation.packLe32(&buf, value);
        try std.testing.expectEqual(value, implementation.unpackLe32(&buf));
    }
}

test "le32 writes the low byte first" {
    var buf: [4]u8 = @splat(0);
    implementation.packLe32(&buf, 0x01020304);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x04, 0x03, 0x02, 0x01 }, &buf);
}

test "serialize then deserialize reproduces every body field" {
    const rec = sampleRecord(1530, implementation.flag_provisioned | implementation.flag_vcom_valid);
    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(&rec, 9, &buf);

    var out: implementation.Record = .{};
    implementation.deserialize(&buf, &out);
    try std.testing.expectEqualSlices(u8, &rec.body.serial, &out.body.serial);
    try std.testing.expectEqualSlices(u8, &rec.body.panel_serial, &out.body.panel_serial);
    try std.testing.expectEqualSlices(u8, &rec.body.panel_lut_id, &out.body.panel_lut_id);
    try std.testing.expectEqualSlices(u8, &rec.body.touch_cal, &out.body.touch_cal);
    try std.testing.expectEqual(rec.body.mfg_date, out.body.mfg_date);
    try std.testing.expectEqual(rec.body.device_key_id, out.body.device_key_id);
    try std.testing.expectEqual(rec.body.hw_rev, out.body.hw_rev);
    try std.testing.expectEqual(rec.body.fixture_id, out.body.fixture_id);
    try std.testing.expectEqual(rec.body.panel_vcom_mv, out.body.panel_vcom_mv);
    try std.testing.expectEqual(rec.flags, out.flags);
    try std.testing.expectEqual(@as(u32, 9), out.seq);
    try std.testing.expectEqual(implementation.schema_ver, out.schema_version);
}

test "serialize stamps the header discriminators at their documented offsets" {
    const rec = sampleRecord(1530, implementation.flag_vcom_valid);
    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(&rec, 3, &buf);

    try std.testing.expectEqual(implementation.magic, implementation.unpackLe32(buf[implementation.off_magic..]));
    try std.testing.expectEqual(implementation.schema_ver, implementation.unpackLe16(buf[implementation.off_schema..]));
    try std.testing.expectEqual(@as(u16, 128), implementation.unpackLe16(buf[implementation.off_reclen..]));
    try std.testing.expectEqual(@as(u32, 3), implementation.unpackLe32(buf[implementation.off_seq..]));
    try std.testing.expectEqual(implementation.flag_vcom_valid, implementation.unpackLe32(buf[implementation.off_flags..]));
}

test "serialize zeroes the reserved header and body padding" {
    const rec = sampleRecord(1530, 0);
    var buf: [implementation.record_len]u8 = @splat(0xAA);
    implementation.serialize(&rec, 1, &buf);
    for (buf[implementation.off_hdr_rsvd..][0..implementation.hdr_rsvd_len]) |byte| {
        try std.testing.expectEqual(@as(u8, 0), byte);
    }
    for (buf[implementation.off_body_rsvd..][0..implementation.body_rsvd_len]) |byte| {
        try std.testing.expectEqual(@as(u8, 0), byte);
    }
}

test "the stored CRC covers the body span only" {
    const rec = sampleRecord(1530, 0);
    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(&rec, 1, &buf);
    const stored = implementation.unpackLe32(buf[implementation.off_crc..]);
    try std.testing.expectEqual(
        stored,
        implementation.crc32(buf[implementation.hdr_bytes..][0..implementation.body_bytes]),
    );
    // A header byte outside the CRC span does not disturb the checksum.
    buf[implementation.off_seq] ^= 0x01;
    try std.testing.expectEqual(
        stored,
        implementation.crc32(buf[implementation.hdr_bytes..][0..implementation.body_bytes]),
    );
}

test "copyValid accepts a freshly serialised record" {
    const rec = sampleRecord(1530, implementation.flag_vcom_valid);
    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(&rec, 1, &buf);
    try std.testing.expect(implementation.copyValid(&buf));
}

test "copyValid rejects a blank window" {
    const buf: [implementation.record_len]u8 = @splat(0xFF);
    try std.testing.expect(!implementation.copyValid(&buf));
}

test "copyValid rejects a wrong magic" {
    const rec = sampleRecord(1530, 0);
    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(&rec, 1, &buf);
    buf[implementation.off_magic] ^= 0x01;
    try std.testing.expect(!implementation.copyValid(&buf));
}

test "copyValid rejects a newer, unknown schema but accepts an older one" {
    const rec = sampleRecord(1530, 0);
    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(&rec, 1, &buf);
    implementation.packLe16(buf[implementation.off_schema..], 99);
    try std.testing.expect(!implementation.copyValid(&buf));
    implementation.packLe16(buf[implementation.off_schema..], 0);
    try std.testing.expect(implementation.copyValid(&buf));
}

test "copyValid rejects a wrong record_len" {
    const rec = sampleRecord(1530, 0);
    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(&rec, 1, &buf);
    buf[implementation.off_reclen] ^= 0x01;
    try std.testing.expect(!implementation.copyValid(&buf));
}

test "copyValid rejects a single-bit body corruption" {
    const rec = sampleRecord(1530, 0);
    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(&rec, 1, &buf);
    buf[implementation.off_serial] ^= 0x01;
    try std.testing.expect(!implementation.copyValid(&buf));
}

test "targetOffset prefers an invalid slot over a valid one" {
    try std.testing.expectEqual(implementation.copy0_off, implementation.targetOffset(false, 0, true, 9));
    try std.testing.expectEqual(implementation.copy1_off, implementation.targetOffset(true, 9, false, 0));
    try std.testing.expectEqual(implementation.copy0_off, implementation.targetOffset(false, 0, false, 0));
}

test "targetOffset overwrites the lower sequence when both are valid" {
    try std.testing.expectEqual(implementation.copy0_off, implementation.targetOffset(true, 1, true, 2));
    try std.testing.expectEqual(implementation.copy1_off, implementation.targetOffset(true, 2, true, 1));
    // A tie targets copy 0, the same side the resolver's tie-break keeps.
    try std.testing.expectEqual(implementation.copy0_off, implementation.targetOffset(true, 4, true, 4));
}

test "nextSeq is one past the newest valid copy" {
    try std.testing.expectEqual(@as(u32, 1), implementation.nextSeq(false, 0, false, 0));
    try std.testing.expectEqual(@as(u32, 2), implementation.nextSeq(true, 1, false, 0));
    try std.testing.expectEqual(@as(u32, 3), implementation.nextSeq(false, 9, true, 2));
    try std.testing.expectEqual(@as(u32, 8), implementation.nextSeq(true, 7, true, 4));
}

test "nextSeq ignores the sequence of an invalid copy" {
    try std.testing.expectEqual(@as(u32, 1), implementation.nextSeq(false, 0xFFFF, false, 0xFFFF));
}

test "vcomInRange brackets the plausible window" {
    try std.testing.expect(implementation.vcomInRange(1530));
    try std.testing.expect(implementation.vcomInRange(implementation.vcom_min_mv));
    try std.testing.expect(implementation.vcomInRange(implementation.vcom_max_mv));
    try std.testing.expect(!implementation.vcomInRange(implementation.vcom_min_mv - 1));
    try std.testing.expect(!implementation.vcomInRange(implementation.vcom_max_mv + 1));
}

test "vcomInRange rejects both real-world poison values" {
    try std.testing.expect(!implementation.vcomInRange(0));
    try std.testing.expect(!implementation.vcomInRange(0xFFFF));
}

test "the layout constants hold the header's stated invariants" {
    try std.testing.expectEqual(implementation.record_len, implementation.hdr_bytes + implementation.body_bytes);
    try std.testing.expectEqual(implementation.sig_headroom, implementation.slot_bytes - implementation.record_len);
    try std.testing.expect(implementation.copy1_off - implementation.copy0_off >= implementation.slot_bytes);
    try std.testing.expect(implementation.copy1_off + implementation.slot_bytes <= 0x200);
    try std.testing.expectEqual(@as(u32, 0), implementation.record_len % implementation.page_bytes);
}
