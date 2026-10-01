//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SOF0 dimension probe.
//!
//! Answers "how big is this JPEG" without decoding it: walk the marker chain,
//! stop at the first SOF0, read the two big-endian shorts. Re-entrant, because
//! it holds no state beyond the caller's cursor.

const spec = @import("spec");

/// What the probe can conclude, mapped to `ra8_err_t` by the ABI membrane.
pub const Error = error{
    /// Not a JPEG, or a segment length that walks off the buffer.
    Protocol,
    /// A frame type this decoder does not implement.
    Unsupported,
};

/// Markers the walk treats specially. The rest are skipped by length.
const Walk = struct {
    /// First and last SOF code; anything in the range other than DHT and JPG
    /// is a frame type we do not decode.
    pub const sof_lo: u16 = 0xFFC0;
    pub const sof_hi: u16 = 0xFFCF;
    pub const dht: u16 = 0xFFC4;
    pub const jpg: u16 = 0xFFC8;

    /// Smallest stream that could carry SOI plus one marker.
    pub const min_len: usize = 4;
    /// A segment always carries its own two length bytes.
    pub const length_bytes: usize = 2;
    /// Offsets into a SOF0 payload, from the length field.
    pub const precision_off: usize = 2;
    pub const height_off: usize = 3;
    pub const width_off: usize = 5;
    /// Shortest SOF0 that still holds precision and both dimensions.
    pub const sof0_min_len: u16 = 8;
};

/// An image's pixel dimensions.
pub const Dimensions = struct {
    width: u16,
    height: u16,
};

fn readBe16(bytes: []const u8) u16 {
    return (@as(u16, bytes[0]) << 8) | bytes[1];
}

/// Read the next marker code, skipping any run of `0xFF` fill bytes.
fn nextMarker(stream: []const u8, cursor: *usize) Error!u16 {
    if (stream[cursor.*] != spec.Marker.stuff_trigger) return Error.Protocol;

    while (cursor.* < stream.len and stream[cursor.*] == spec.Marker.stuff_trigger) {
        cursor.* += 1;
    }
    if (cursor.* >= stream.len) return Error.Protocol;

    const low = stream[cursor.*];
    cursor.* += 1;
    return (@as(u16, spec.Marker.stuff_trigger) << 8) | low;
}

/// Pull the dimensions out of a SOF0 payload starting at its length field.
fn parseSof0(payload: []const u8, seglen: u16) Error!Dimensions {
    if (seglen < Walk.sof0_min_len) return Error.Protocol;
    if (payload[Walk.precision_off] != spec.Segment.precision) return Error.Unsupported;

    const found = Dimensions{
        .height = readBe16(payload[Walk.height_off..]),
        .width = readBe16(payload[Walk.width_off..]),
    };
    if (found.width == 0 or found.height == 0) return Error.Protocol;
    return found;
}

/// One marker of the walk. Returns the dimensions once SOF0 is reached.
fn step(stream: []const u8, cursor: *usize) Error!?Dimensions {
    const marker = try nextMarker(stream, cursor);

    // SOI and EOI carry no payload, so there is no length to skip.
    if (marker == spec.Marker.soi or marker == spec.Marker.eoi) return null;

    if (cursor.* + Walk.length_bytes > stream.len) return Error.Protocol;
    const seglen = readBe16(stream[cursor.*..]);
    if (seglen < Walk.length_bytes or seglen > stream.len - cursor.*) return Error.Protocol;

    if (marker == spec.Marker.sof0) return try parseSof0(stream[cursor.*..], seglen);

    if (marker >= Walk.sof_lo and marker <= Walk.sof_hi and
        marker != Walk.dht and marker != Walk.jpg)
    {
        return Error.Unsupported;
    }

    cursor.* += seglen;
    return null;
}

/// Walk `stream` to its first SOF0. Errors when the stream is not a baseline
/// JPEG, or ends before one is found.
pub fn probe(stream: []const u8) Error!Dimensions {
    if (readBe16(stream) != spec.Marker.soi) return Error.Protocol;

    var cursor: usize = Walk.length_bytes;
    while (cursor + Walk.min_len <= stream.len) {
        if (try step(stream, &cursor)) |found| return found;
    }
    return Error.Protocol;
}

/// Shortest stream the probe will look at; below this the caller reports a
/// size error rather than a protocol one.
pub const min_stream_len: usize = Walk.min_len;
