//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Minimal baseline JPEG construction for the `gen_jpeg_fixture` build tool
//! (#858), replacing the Python implementation this change deletes.
//!
//! Dimensions in, bytes out: no file system and no process state, so the blob
//! the libFuzzer corpora are seeded with is provable without writing a file.
//!
//! The blob only has to satisfy the parser in `libs/ra8_hal/src/ra8_jpeg_sw.c`
//! well enough that a fuzz run starts from real coverage rather than from the
//! reject path: a valid SOI, a SOF0 whose width and height parse, and entropy
//! bytes followed by EOI. It is NOT a decodable image, and the DCT
//! coefficients carry no meaning. Every seed is therefore the same 346 bytes
//! apart from the four dimension bytes inside SOF0, which is what makes the
//! swap from Python differentially checkable byte for byte.
//!
//! Segment order, fixed: SOI, APP0 (JFIF 1.01), DQT (8-bit luma, all ones),
//! SOF0 (baseline, 8-bit, one Y component), DHT (DC luma), DHT (AC luma), SOS
//! (one component), sixteen entropy bytes, EOI.

const std = @import("std");

/// Smallest dimension a JPEG SOF0 field can carry. Zero is not a picture, and
/// the parser under test rejects it before the interesting paths.
pub const min_dimension: u32 = 1;

/// Largest dimension a JPEG SOF0 field can carry: the field is 16-bit.
pub const max_dimension: u32 = 0xFFFF;

/// Every seed is this long. The segments are fixed and only the four SOF0
/// dimension bytes vary, so the length is a constant rather than a sum
/// computed per call, and the tests pin it.
pub const blob_len: usize = 346;

pub const Error = error{DimensionOutOfRange};

const marker_soi: u8 = 0xD8;
const marker_app0: u8 = 0xE0;
const marker_dqt: u8 = 0xDB;
const marker_sof0: u8 = 0xC0;
const marker_dht: u8 = 0xC4;
const marker_sos: u8 = 0xDA;
const marker_eoi: u8 = 0xD9;

/// JFIF 1.01, aspect-ratio density 1:1, no thumbnail.
const app0_payload = [_]u8{
    'J',  'F',  'I',  'F',  0x00,
    0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00,
};

/// One 8-bit luma quantisation table, every coefficient 1. Not a sensible
/// table; a parseable one.
const dqt_payload: [65]u8 = blk: {
    var payload: [65]u8 = undefined;
    payload[0] = 0x00;
    for (payload[1..]) |*slot| slot.* = 0x01;
    break :blk payload;
};

/// DC luma Huffman table: class 0, table 0, sixteen code-length counts, then
/// the twelve symbols those counts describe.
const dht_dc_payload: [29]u8 = blk: {
    var payload: [29]u8 = undefined;
    payload[0] = 0x00;
    const counts = [_]u8{ 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
    @memcpy(payload[1..17], &counts);
    for (0..12) |index| payload[17 + index] = @intCast(index);
    break :blk payload;
};

/// AC luma Huffman table: class 1, table 0, sixteen counts, then the 162
/// symbols the standard's example table carries.
const dht_ac_payload: [179]u8 = blk: {
    var payload: [179]u8 = undefined;
    payload[0] = 0x10;
    const counts = [_]u8{ 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D };
    @memcpy(payload[1..17], &counts);
    for (0..0xA2) |index| payload[17 + index] = @intCast(index);
    break :blk payload;
};

/// Scan header: one component, DC and AC table 0, full spectral selection.
const sos_payload = [_]u8{ 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00 };

/// Entropy-coded data. Deliberately all zero: a 0xFF byte inside the entropy
/// segment would need 0x00 stuffing behind it, and avoiding 0xFF entirely
/// keeps the stream trivially well formed.
const entropy = [_]u8{0x00} ** 16;

/// Append a marker segment: 0xFF, the marker, the big-endian length including
/// the two length bytes themselves, then the payload.
fn appendSegment(out: *std.ArrayList(u8), marker: u8, payload: []const u8) !void {
    const length: u16 = @intCast(payload.len + 2);
    try out.appendSlice(&[_]u8{ 0xFF, marker });
    try out.appendSlice(&[_]u8{ @intCast(length >> 8), @truncate(length) });
    try out.appendSlice(payload);
}

/// Build one minimal baseline JPEG carrying `width` x `height` in its SOF0.
///
/// Returns `Error.DimensionOutOfRange` when either dimension falls outside
/// 1..65535, which is the whole of what the SOF0 field can express. The
/// Python implementation raised `ValueError` on the same condition and let it
/// reach the process boundary; `cli.zig` turns this error into the same exit
/// status.
///
/// The returned slice is owned by the caller.
pub fn buildMinimalJpeg(allocator: std.mem.Allocator, width: u32, height: u32) ![]u8 {
    if (width < min_dimension or width > max_dimension) return Error.DimensionOutOfRange;
    if (height < min_dimension or height > max_dimension) return Error.DimensionOutOfRange;

    var out = try std.ArrayList(u8).initCapacity(allocator, blob_len);
    errdefer out.deinit();

    try out.appendSlice(&[_]u8{ 0xFF, marker_soi });
    try appendSegment(&out, marker_app0, &app0_payload);
    try appendSegment(&out, marker_dqt, &dqt_payload);

    const height16: u16 = @intCast(height);
    const width16: u16 = @intCast(width);
    const sof0_payload = [_]u8{
        0x08,
        @intCast(height16 >> 8),
        @truncate(height16),
        @intCast(width16 >> 8),
        @truncate(width16),
        0x01,
        0x01,
        0x11,
        0x00,
    };
    try appendSegment(&out, marker_sof0, &sof0_payload);

    try appendSegment(&out, marker_dht, &dht_dc_payload);
    try appendSegment(&out, marker_dht, &dht_ac_payload);
    try appendSegment(&out, marker_sos, &sos_payload);
    try out.appendSlice(&entropy);
    try out.appendSlice(&[_]u8{ 0xFF, marker_eoi });

    return out.toOwnedSlice();
}
