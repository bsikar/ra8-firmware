//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the `ra8_epd_cal` record codec: CRC-32, little-endian
//! packing, the serialise / deserialise round trip and every refusal the
//! decoder can reach.

const std = @import("std");
const implementation = @import("implementation");

test "crc32 matches the IEEE 802.3 check vector" {
    try std.testing.expectEqual(@as(u32, 0xCBF43926), implementation.crc32("123456789"));
    try std.testing.expectEqual(@as(u32, 0), implementation.crc32(""));
}

test "little-endian packing round-trips" {
    var two: [2]u8 = @splat(0);
    implementation.packLe16(&two, 0x1234);
    try std.testing.expectEqual(@as(u8, 0x34), two[0]);
    try std.testing.expectEqual(@as(u8, 0x12), two[1]);
    try std.testing.expectEqual(@as(u16, 0x1234), implementation.unpackLe16(&two));

    var four: [4]u8 = @splat(0);
    implementation.packLe32(&four, 0xDEADBEEF);
    try std.testing.expectEqual(@as(u8, 0xEF), four[0]);
    try std.testing.expectEqual(@as(u8, 0xDE), four[3]);
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), implementation.unpackLe32(&four));
}

test "serialize lays the record out at the documented offsets" {
    var blob: [implementation.blob_size]u8 = @splat(0xAA);
    try implementation.serialize(.{ .vcom_mv = 1530, .schema_version = 1 }, &blob);

    try std.testing.expectEqualSlices(u8, &implementation.magic, blob[0..4]);
    try std.testing.expectEqual(implementation.schema_version, blob[implementation.offset.version]);
    try std.testing.expectEqual(@as(u8, 0), blob[implementation.offset.reserved0]);
    try std.testing.expectEqual(
        implementation.payload_len,
        implementation.unpackLe16(blob[implementation.offset.payload_len..][0..2]),
    );
    try std.testing.expectEqual(
        @as(u16, 1530),
        implementation.unpackLe16(blob[implementation.offset.vcom_mv..][0..2]),
    );
    // The reserved growth span stays zeroed for a future schema to claim.
    try std.testing.expect(std.mem.allEqual(u8, blob[implementation.offset.reserved1..implementation.offset.crc32], 0));
}

test "serialize refuses a short buffer and a zero VCOM" {
    var short: [implementation.blob_size - 1]u8 = @splat(0);
    try std.testing.expectError(
        error.ShortBuffer,
        implementation.serialize(.{ .vcom_mv = 1530, .schema_version = 1 }, &short),
    );

    var blob: [implementation.blob_size]u8 = @splat(0);
    try std.testing.expectError(
        error.ZeroVcom,
        implementation.serialize(.{ .vcom_mv = 0, .schema_version = 1 }, &blob),
    );
}

test "deserialize round-trips a serialized record" {
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(.{ .vcom_mv = 2050, .schema_version = 1 }, &blob);

    const decoded = try implementation.deserialize(&blob);
    try std.testing.expectEqual(@as(u16, 2050), decoded.vcom_mv);
    try std.testing.expectEqual(implementation.schema_version, decoded.schema_version);
}

test "deserialize reports blank storage as not found" {
    const blank: [implementation.blob_size]u8 = @splat(0xFF);
    try std.testing.expectError(error.NotFound, implementation.deserialize(&blank));

    const zeroed: [implementation.blob_size]u8 = @splat(0);
    try std.testing.expectError(error.NotFound, implementation.deserialize(&zeroed));
}

test "deserialize refuses a short buffer" {
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(.{ .vcom_mv = 1530, .schema_version = 1 }, &blob);
    try std.testing.expectError(
        error.ShortBuffer,
        implementation.deserialize(blob[0 .. implementation.blob_size - 1]),
    );
}

test "deserialize refuses a newer schema" {
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(.{ .vcom_mv = 1530, .schema_version = 1 }, &blob);
    blob[implementation.offset.version] = implementation.schema_version + 1;
    implementation.packLe32(
        blob[implementation.offset.crc32..][0..4],
        implementation.crc32(blob[0..implementation.offset.crc32]),
    );
    try std.testing.expectError(error.UnsupportedSchema, implementation.deserialize(&blob));
}

test "deserialize refuses a foreign payload length" {
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(.{ .vcom_mv = 1530, .schema_version = 1 }, &blob);
    implementation.packLe16(blob[implementation.offset.payload_len..][0..2], implementation.payload_len + 1);
    implementation.packLe32(
        blob[implementation.offset.crc32..][0..4],
        implementation.crc32(blob[0..implementation.offset.crc32]),
    );
    try std.testing.expectError(error.ValidationFailed, implementation.deserialize(&blob));
}

test "deserialize catches a damaged body" {
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(.{ .vcom_mv = 1530, .schema_version = 1 }, &blob);
    blob[implementation.offset.vcom_mv] ^= 0x01;
    try std.testing.expectError(error.CrcMismatch, implementation.deserialize(&blob));
}

test "deserialize refuses a CRC-valid zero VCOM" {
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(.{ .vcom_mv = 1530, .schema_version = 1 }, &blob);
    implementation.packLe16(blob[implementation.offset.vcom_mv..][0..2], 0);
    implementation.packLe32(
        blob[implementation.offset.crc32..][0..4],
        implementation.crc32(blob[0..implementation.offset.crc32]),
    );
    try std.testing.expectError(error.ValidationFailed, implementation.deserialize(&blob));
}

test "vcomInRange holds the window inclusive at both ends" {
    const limits: implementation.Limits = .{ .min_mv = 200, .max_mv = 4000 };
    try std.testing.expect(implementation.vcomInRange(200, limits));
    try std.testing.expect(implementation.vcomInRange(4000, limits));
    try std.testing.expect(!implementation.vcomInRange(199, limits));
    try std.testing.expect(!implementation.vcomInRange(4001, limits));
    // The two failure signatures that matter in practice.
    try std.testing.expect(!implementation.vcomInRange(0, limits));
    try std.testing.expect(!implementation.vcomInRange(0xFFFF, limits));
}

test "limitsUsable rejects zero and inverted windows" {
    try std.testing.expect(implementation.limitsUsable(.{ .min_mv = 200, .max_mv = 4000 }));
    try std.testing.expect(implementation.limitsUsable(.{ .min_mv = 1530, .max_mv = 1530 }));
    try std.testing.expect(!implementation.limitsUsable(.{ .min_mv = 0, .max_mv = 4000 }));
    try std.testing.expect(!implementation.limitsUsable(.{ .min_mv = 4000, .max_mv = 200 }));
}

test "magicOk only accepts the EVCM word" {
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(.{ .vcom_mv = 1530, .schema_version = 1 }, &blob);
    try std.testing.expect(implementation.magicOk(&blob));
    blob[3] = 'X';
    try std.testing.expect(!implementation.magicOk(&blob));
    try std.testing.expect(!implementation.magicOk(blob[0..3]));
}
