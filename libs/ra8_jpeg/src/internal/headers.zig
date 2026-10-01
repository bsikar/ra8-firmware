//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! JFIF header segments for the baseline encoder: every byte that
//! precedes the entropy-coded scan of a 4:2:0 YCbCr stream, in JFIF 1.1
//! order. Output is fixed-shape, so each writer takes only what varies.

const spec = @import("spec");
const huffman = @import("huffman");
const sink_mod = @import("sink");

const Sink = sink_mod.Sink;

/// SOI followed by a minimal APP0 JFIF 1.1 header: no density units, 1:1
/// aspect, no thumbnail. Common viewers reject a stream without it.
fn emitStartAndJfif(sink: *Sink) void {
    sink.word(spec.Marker.soi);

    sink.word(spec.Marker.app0);
    sink.word(spec.Segment.app0_len);
    for ("JFIF") |c| sink.byte(c);
    sink.byte(0);
    sink.byte(1); // Major version.
    sink.byte(1); // Minor version.
    sink.byte(0); // No density units.
    sink.word(1); // X density.
    sink.word(1); // Y density.
    sink.byte(0); // Thumbnail width.
    sink.byte(0); // Thumbnail height.
}

/// Both quantisation tables, written in zig-zag order as T.81 sec B.2.4.1
/// requires, at 8-bit precision (Pq = 0).
fn emitQuantTables(
    sink: *Sink,
    luma: *const [spec.Block.size]u8,
    chroma: *const [spec.Block.size]u8,
) void {
    const payload: u16 = 2 + (2 * (1 + @as(u16, spec.Block.size)));
    sink.word(spec.Marker.dqt);
    sink.word(payload);
    for ([_]*const [spec.Block.size]u8{ luma, chroma }, 0..) |table, identifier| {
        sink.byte(@intCast(identifier)); // PqTq: 8-bit precision, table id.
        for (spec.zigzag) |raster| sink.byte(table[raster]);
    }
}

/// Baseline frame header: three components, luma 2x2 and chroma 1x1.
fn emitFrame(sink: *Sink, width: u16, height: u16) void {
    sink.word(spec.Marker.sof0);
    sink.word(spec.Segment.sof0_len);
    sink.byte(spec.Segment.precision);
    sink.word(height);
    sink.word(width);
    sink.byte(spec.Segment.components);

    const components = [_]struct { id: u8, sampling: u8, quant: u8 }{
        .{ .id = 1, .sampling = spec.Segment.sampling_luma, .quant = 0 },
        .{ .id = 2, .sampling = spec.Segment.sampling_chroma, .quant = 1 },
        .{ .id = 3, .sampling = spec.Segment.sampling_chroma, .quant = 1 },
    };
    for (components) |component| {
        sink.byte(component.id);
        sink.byte(component.sampling);
        sink.byte(component.quant);
    }
}

/// One DHT segment for one table.
fn emitHuffmanTable(
    sink: *Sink,
    class_and_id: u8,
    specification: huffman.Specification,
    total: u16,
) void {
    sink.word(spec.Marker.dht);
    sink.word(2 + 1 + @as(u16, spec.Huff.lengths) + total);
    sink.byte(class_and_id);
    for (specification.bits) |count| sink.byte(count);
    for (specification.values[0..total]) |symbol| sink.byte(symbol);
}

/// Scan header: all three components, full spectral band, no successive
/// approximation.
fn emitScan(sink: *Sink) void {
    sink.word(spec.Marker.sos);
    sink.word(spec.Segment.sos_len);
    sink.byte(spec.Segment.components);
    sink.byte(1);
    sink.byte(spec.Segment.sos_luma);
    sink.byte(2);
    sink.byte(spec.Segment.sos_chroma);
    sink.byte(3);
    sink.byte(spec.Segment.sos_chroma);
    sink.byte(0); // Spectral selection start.
    sink.byte(spec.Segment.spectral_end);
    sink.byte(0); // Successive approximation.
}

/// Write every header segment, in order, up to the start of the scan.
pub fn emitAll(
    sink: *Sink,
    width: u16,
    height: u16,
    luma: *const [spec.Block.size]u8,
    chroma: *const [spec.Block.size]u8,
    tables: *const huffman.Set,
) void {
    emitStartAndJfif(sink);
    emitQuantTables(sink, luma, chroma);
    emitFrame(sink, width, height);

    emitHuffmanTable(sink, spec.Segment.dht_dc_luma, huffman.dc_luma, tables.total_dc_luma);
    emitHuffmanTable(sink, spec.Segment.dht_ac_luma, huffman.ac_luma, tables.total_ac_luma);
    emitHuffmanTable(sink, spec.Segment.dht_dc_chroma, huffman.dc_chroma, tables.total_dc_chroma);
    emitHuffmanTable(sink, spec.Segment.dht_ac_chroma, huffman.ac_chroma, tables.total_ac_chroma);

    emitScan(sink);
}
