//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Decision tests for the baseline JPEG encoder.
//!
//! These drive the units that decide bytes: the IJG quality curve, the
//! quantisation clamps and rounding, the SSSS magnitude categories, the
//! Annex C canonical Huffman build, and the bit sink's stuffing, padding and
//! overflow latch. Byte-exactness of a whole stream against the C encoder was
//! checked on the host at port time; what is pinned here is the arithmetic
//! those bytes come from.

const std = @import("std");
const testing = std.testing;

const spec = @import("spec");
const quant = @import("quant");
const huffman = @import("huffman");
const entropy = @import("entropy");
const sink_mod = @import("sink");
const dct = @import("dct");

test "zigzag is a permutation of the 64 raster indices" {
    var seen: [spec.Block.size]bool = @splat(false);
    for (spec.zigzag) |index| {
        try testing.expect(index < spec.Block.size);
        try testing.expect(!seen[index]);
        seen[index] = true;
    }
    try testing.expectEqual(@as(u8, 0), spec.zigzag[0]);
}

test "quality curve pivots at 50 and reaches zero at 100" {
    try testing.expectEqual(@as(u16, 5000), quant.qualityScale(1));
    try testing.expectEqual(@as(u16, 200), quant.qualityScale(25));
    try testing.expectEqual(@as(u16, 100), quant.qualityScale(50));
    try testing.expectEqual(@as(u16, 50), quant.qualityScale(75));
    try testing.expectEqual(@as(u16, 20), quant.qualityScale(90));
    try testing.expectEqual(@as(u16, 0), quant.qualityScale(100));
}

test "scaled tables stay inside the legal DQT range" {
    var table: [spec.Block.size]u8 = undefined;

    quant.scaleTable(&quant.base_luma, &table, quant.qualityScale(100));
    for (table) |entry| try testing.expectEqual(@as(u8, 1), entry);

    quant.scaleTable(&quant.base_luma, &table, quant.qualityScale(1));
    for (table) |entry| try testing.expectEqual(@as(u8, 255), entry);

    quant.scaleTable(&quant.base_luma, &table, quant.qualityScale(50));
    try testing.expectEqualSlices(u8, &quant.base_luma, &table);
}

test "quantisation rounds half away from zero, symmetrically" {
    try testing.expectEqual(@as(i32, 0), quant.apply(4, 10));
    try testing.expectEqual(@as(i32, 1), quant.apply(5, 10));
    try testing.expectEqual(@as(i32, 0), quant.apply(-4, 10));
    try testing.expectEqual(@as(i32, -1), quant.apply(-5, 10));
    try testing.expectEqual(@as(i32, 3), quant.apply(25, 8));
    try testing.expectEqual(@as(i32, -3), quant.apply(-25, 8));
}

test "a zero divisor is treated as one rather than dividing by zero" {
    try testing.expectEqual(@as(i32, 77), quant.apply(77, 0));
    try testing.expectEqual(@as(i32, -77), quant.apply(-77, 0));
}

test "magnitude category is the bit length of the absolute value" {
    try testing.expectEqual(@as(u8, 0), entropy.magnitudeBits(0));
    try testing.expectEqual(@as(u8, 1), entropy.magnitudeBits(1));
    try testing.expectEqual(@as(u8, 1), entropy.magnitudeBits(-1));
    try testing.expectEqual(@as(u8, 2), entropy.magnitudeBits(2));
    try testing.expectEqual(@as(u8, 2), entropy.magnitudeBits(-3));
    try testing.expectEqual(@as(u8, 8), entropy.magnitudeBits(255));
    try testing.expectEqual(@as(u8, 11), entropy.magnitudeBits(-1024));
    try testing.expectEqual(@as(u8, 32), entropy.magnitudeBits(std.math.minInt(i32)));
}

test "every K.3.3 specification's BITS list matches its symbol count" {
    for ([_]huffman.Specification{
        huffman.dc_luma,
        huffman.ac_luma,
        huffman.dc_chroma,
        huffman.ac_chroma,
    }) |specification| {
        try testing.expectEqual(specification.values.len, specification.total());
    }
    try testing.expectEqual(@as(u16, 12), huffman.dc_luma.total());
    try testing.expectEqual(@as(u16, 162), huffman.ac_luma.total());
}

test "canonical build gives the T.81 Annex K.3.3 luma DC codes" {
    var table = huffman.Table{};
    try testing.expectEqual(@as(u16, 12), huffman.build(&table, huffman.dc_luma));

    // K.3.3 Table K.3: one 2-bit code, five 3-bit, then one per length 4..9.
    const expected = [_]struct { symbol: u8, code: u16, size: u8 }{
        .{ .symbol = 0, .code = 0x00, .size = 2 },
        .{ .symbol = 1, .code = 0x02, .size = 3 },
        .{ .symbol = 5, .code = 0x06, .size = 3 },
        .{ .symbol = 6, .code = 0x0E, .size = 4 },
        .{ .symbol = 7, .code = 0x1E, .size = 5 },
        .{ .symbol = 11, .code = 0x1FE, .size = 9 },
    };
    for (expected) |row| {
        try testing.expectEqual(row.code, table.codes[row.symbol]);
        try testing.expectEqual(row.size, table.sizes[row.symbol]);
    }
}

test "canonical build gives the T.81 Annex K.3.3 luma AC anchors" {
    var table = huffman.Table{};
    try testing.expectEqual(@as(u16, 162), huffman.build(&table, huffman.ac_luma));

    // EOB and ZRL are the two symbols the entropy coder reaches by name.
    try testing.expectEqual(@as(u16, 0x0A), table.codes[spec.Huff.eob]);
    try testing.expectEqual(@as(u8, 4), table.sizes[spec.Huff.eob]);
    try testing.expectEqual(@as(u16, 0x7F9), table.codes[spec.Huff.zrl]);
    try testing.expectEqual(@as(u8, 11), table.sizes[spec.Huff.zrl]);
}

test "a symbol absent from a specification keeps length zero" {
    var table = huffman.Table{};
    _ = huffman.build(&table, huffman.dc_luma);
    // DC tables define 0..11 only.
    try testing.expectEqual(@as(u8, 0), table.sizes[12]);
    try testing.expectEqual(@as(u8, 0), table.sizes[255]);
}

test "every table's codes are prefix-free within their lengths" {
    for ([_]huffman.Specification{
        huffman.dc_luma,
        huffman.ac_luma,
        huffman.dc_chroma,
        huffman.ac_chroma,
    }) |specification| {
        var table = huffman.Table{};
        _ = huffman.build(&table, specification);
        for (specification.values) |a| {
            for (specification.values) |b| {
                if (a == b) continue;
                const shorter = @min(table.sizes[a], table.sizes[b]);
                const shift_a: u4 = @intCast(table.sizes[a] - shorter);
                const shift_b: u4 = @intCast(table.sizes[b] - shorter);
                try testing.expect((table.codes[a] >> shift_a) != (table.codes[b] >> shift_b));
            }
        }
    }
}

test "the sink writes big-endian words and latches overflow" {
    var buffer: [3]u8 = undefined;
    var sink = sink_mod.Sink{ .dst = &buffer };

    sink.word(0xFFD8);
    try testing.expectEqualSlices(u8, &.{ 0xFF, 0xD8 }, buffer[0..2]);
    try testing.expect(!sink.overflow);

    sink.word(0xABCD);
    try testing.expect(sink.overflow);
    try testing.expectEqual(@as(usize, 3), sink.pos);
}

test "an emitted 0xFF is followed by a stuffed zero" {
    var buffer: [8]u8 = undefined;
    var sink = sink_mod.Sink{ .dst = &buffer };

    sink.bits(0xFF, 8);
    try testing.expectEqual(@as(usize, 2), sink.pos);
    try testing.expectEqualSlices(u8, &.{ 0xFF, 0x00 }, buffer[0..2]);
}

test "a partial trailing byte is one-padded on flush" {
    var buffer: [4]u8 = undefined;
    var sink = sink_mod.Sink{ .dst = &buffer };

    sink.bits(0b101, 3);
    try testing.expectEqual(@as(usize, 0), sink.pos);
    sink.flushBits();
    try testing.expectEqual(@as(usize, 1), sink.pos);
    try testing.expectEqual(@as(u8, 0b10111111), buffer[0]);
    try testing.expectEqual(@as(u8, 0), sink.bit_cnt);
}

test "flushing an already-aligned stream writes nothing" {
    var buffer: [4]u8 = undefined;
    var sink = sink_mod.Sink{ .dst = &buffer };

    sink.bits(0xA5, 8);
    try testing.expectEqual(@as(usize, 1), sink.pos);
    sink.flushBits();
    try testing.expectEqual(@as(usize, 1), sink.pos);
}

test "a push wider than the accumulator fails closed" {
    var buffer: [8]u8 = undefined;
    var sink = sink_mod.Sink{ .dst = &buffer };

    sink.bits(0, 0);
    try testing.expectEqual(@as(usize, 0), sink.pos);
    try testing.expect(!sink.overflow);

    sink.bits(0xFFFFFFFF, sink_mod.Sink.max_push_bits + 1);
    try testing.expect(sink.overflow);
    try testing.expectEqual(@as(usize, 0), sink.pos);
}

test "the forward DCT puts a flat block entirely in the DC term" {
    var block: [spec.Block.size]i32 = @splat(100);
    dct.forward(&block);

    try testing.expectEqual(@as(i32, 800), block[0]);
    for (block[1..]) |coefficient| {
        try testing.expectEqual(@as(i32, 0), coefficient);
    }
}

test "the forward DCT leaves a zero block at zero" {
    var block: [spec.Block.size]i32 = @splat(0);
    dct.forward(&block);
    for (block) |coefficient| try testing.expectEqual(@as(i32, 0), coefficient);
}
