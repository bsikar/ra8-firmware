//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Record codec for `ra8_epd_cal`: the CRC-32 checksum, the little-endian
//! packing helpers and the serialise / deserialise pair, kept free of the
//! injected seams so every branch is reachable from a plain unit test.
//!
//! The storage format is unsigned and lives outside the DFU-signed image, so
//! it carries its own integrity check. Byte order is a property of the record
//! rather than of the compiler that wrote it, hence the explicit packing.

const std = @import("std");

/// Serialised record size in bytes (`k_ra8_epd_cal_blob_size`).
pub const blob_size: usize = 32;
/// Current on-flash schema version (`k_ra8_epd_cal_schema_version`).
pub const schema_version: u8 = 1;
/// Schema-1 payload bytes, the VCOM magnitude (`k_ra8_epd_cal_payload_len`).
pub const payload_len: u16 = 2;

/// Byte offsets inside the serialised record (`ra8_epd_cal_layout_t`).
pub const offset = struct {
    pub const magic: usize = 0;
    pub const version: usize = 4;
    pub const reserved0: usize = 5;
    pub const payload_len: usize = 6;
    pub const vcom_mv: usize = 8;
    pub const reserved1: usize = 10;
    pub const crc32: usize = 28;
};

/// `'EVCM'`, the record's leading magic word (`ra8_epd_cal_magic_t`).
pub const magic = [4]u8{ 0x45, 0x56, 0x43, 0x4D };

/// IEEE 802.3 CRC-32 parameters, shared with `ra8_touch_cal`.
pub const crc_init: u32 = 0xFFFFFFFF;
/// Reversed IEEE 802.3 polynomial.
pub const crc_poly: u32 = 0xEDB88320;

/// Decoded per-device calibration record (`ra8_epd_cal_record_t`).
pub const Record = extern struct {
    /// VCOM magnitude in millivolts; the sign is implicit.
    vcom_mv: u16 = 0,
    /// Schema the record was written under.
    schema_version: u8 = 0,
};

/// Panel's documented VCOM window (`ra8_epd_cal_limits_mv_t`).
pub const Limits = extern struct {
    /// Lowest accepted VCOM magnitude, millivolts.
    min_mv: u16 = 0,
    /// Highest accepted VCOM magnitude, millivolts.
    max_mv: u16 = 0,
};

/// Why a decode was refused. The resolver treats every variant the same way,
/// but the codec's C callers map each to its own `ra8_err_t`.
pub const DecodeError = error{
    /// Fewer than `blob_size` bytes were offered.
    ShortBuffer,
    /// No `'EVCM'` magic: blank or never-written storage.
    NotFound,
    /// Written by a newer schema than this build understands.
    UnsupportedSchema,
    /// Payload length disagrees with the schema.
    ValidationFailed,
    /// Magic present, body damaged.
    CrcMismatch,
};

/// Why an encode was refused.
pub const EncodeError = error{
    /// Destination is smaller than `blob_size`.
    ShortBuffer,
    /// A zero VCOM is never a legitimate calibration.
    ZeroVcom,
};

/// Compute the IEEE 802.3 CRC-32 of a byte span.
///
/// Bit-banged reflected-polynomial form. No lookup table, because a build
/// that checksums 32 bytes once per boot should not carry a 1 KiB table.
pub fn crc32(data: []const u8) u32 {
    var crc: u32 = crc_init;
    for (data) |byte| {
        crc ^= byte;
        var bit: u8 = 0;
        while (bit < 8) : (bit += 1) {
            const mask: u32 = @bitCast(-%@as(i32, @intCast(crc & 1)));
            crc = (crc >> 1) ^ (crc_poly & mask);
        }
    }
    return crc ^ crc_init;
}

/// Store a little-endian 16-bit value.
pub fn packLe16(dst: *[2]u8, value: u16) void {
    std.mem.writeInt(u16, dst, value, .little);
}

/// Load a little-endian 16-bit value.
pub fn unpackLe16(src: *const [2]u8) u16 {
    return std.mem.readInt(u16, src, .little);
}

/// Store a little-endian 32-bit value.
pub fn packLe32(dst: *[4]u8, value: u32) void {
    std.mem.writeInt(u32, dst, value, .little);
}

/// Load a little-endian 32-bit value.
pub fn unpackLe32(src: *const [4]u8) u32 {
    return std.mem.readInt(u32, src, .little);
}

/// Report whether a serialised record carries the `'EVCM'` magic.
pub fn magicOk(src: []const u8) bool {
    if (src.len < magic.len) return false;
    return std.mem.eql(u8, src[offset.magic..][0..magic.len], &magic);
}

/// Range-check a candidate VCOM against a panel's documented window.
///
/// Rejects the two failure signatures that matter in practice: `0` from a
/// controller that has not finished booting, and `0xFFFF` from blank flash,
/// both by way of the caller's non-zero `min_mv`.
pub fn vcomInRange(mv: u16, limits: Limits) bool {
    return (mv >= limits.min_mv) and (mv <= limits.max_mv);
}

/// Reject limits that cannot decide anything: zero or inverted.
pub fn limitsUsable(limits: Limits) bool {
    return (limits.min_mv != 0) and (limits.min_mv <= limits.max_mv);
}

/// Serialise `record` into `dst`, CRC trailer included.
pub fn serialize(record: Record, dst: []u8) EncodeError!void {
    if (dst.len < blob_size) return EncodeError.ShortBuffer;
    if (record.vcom_mv == 0) return EncodeError.ZeroVcom;

    const blob = dst[0..blob_size];
    @memset(blob, 0);
    @memcpy(blob[offset.magic..][0..magic.len], &magic);
    blob[offset.version] = schema_version;
    packLe16(blob[offset.payload_len..][0..2], payload_len);
    packLe16(blob[offset.vcom_mv..][0..2], record.vcom_mv);
    packLe32(blob[offset.crc32..][0..4], crc32(blob[0..offset.crc32]));
}

/// Decode and validate a serialised record.
pub fn deserialize(src: []const u8) DecodeError!Record {
    if (src.len < blob_size) return DecodeError.ShortBuffer;
    if (!magicOk(src)) return DecodeError.NotFound;

    const version = src[offset.version];
    if (version > schema_version) return DecodeError.UnsupportedSchema;
    if (unpackLe16(src[offset.payload_len..][0..2]) != payload_len) {
        return DecodeError.ValidationFailed;
    }

    const want = unpackLe32(src[offset.crc32..][0..4]);
    const have = crc32(src[0..offset.crc32]);
    if (want != have) return DecodeError.CrcMismatch;

    const vcom = unpackLe16(src[offset.vcom_mv..][0..2]);
    // A CRC-valid record cannot legitimately hold zero, since `serialize`
    // refuses to write one, so this is a writer from another schema.
    if (vcom == 0) return DecodeError.ValidationFailed;

    return .{ .vcom_mv = vcom, .schema_version = version };
}

comptime {
    if (@sizeOf(Record) != 4) @compileError("ra8_epd_cal_record_t size");
    if (@offsetOf(Record, "vcom_mv") != 0) @compileError("ra8_epd_cal_record_t vcom offset");
    if (@offsetOf(Record, "schema_version") != 2) @compileError("ra8_epd_cal_record_t schema offset");
    if (@sizeOf(Limits) != 4) @compileError("ra8_epd_cal_limits_mv_t size");
    if (@offsetOf(Limits, "max_mv") != 2) @compileError("ra8_epd_cal_limits_mv_t max offset");
}
