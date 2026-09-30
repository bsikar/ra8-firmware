//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Marker-segment parsers for the baseline decoder (#2799).
//!
//! One function per segment type the decoder understands, each advancing the
//! context cursor past what it consumed. None of them touch the entropy
//! stream; that starts where `parseSos` leaves the cursor.

const dec_ctx = @import("dec_ctx");
const spec = @import("spec");

const Ctx = dec_ctx.Ctx;
const Error = dec_ctx.Error;
const Nibble = dec_ctx.Nibble;
const Limit = dec_ctx.Limit;

/// Field widths inside the segments this module reads.
const Field = struct {
    /// Every segment starts with its own two length bytes.
    pub const length_bytes: u16 = 2;
    /// SOF0 needs precision, both dimensions and a component count.
    pub const sof0_min: u16 = 8;
    /// SOS needs a component count and at least one selector pair.
    pub const sos_min: u16 = 6;
    /// Bytes each component contributes to a frame header.
    pub const sof0_per_comp: u32 = 3;
    /// 8-bit samples are the only precision baseline defines.
    pub const precision: u8 = 8;
};

/// Skip a segment this decoder has no use for, by its own length.
pub fn skip(d: *Ctx) Error!void {
    const len = try d.segmentLen(Field.length_bytes);
    d.cursor += len;
}

/// Read a DQT segment: one or more 8-bit quantisation tables, stored in
/// zig-zag order and kept in natural order.
pub fn parseDqt(d: *Ctx) Error!void {
    const len = try d.segmentLen(Field.length_bytes);
    const end = d.cursor + len;
    d.cursor += Field.length_bytes;

    while (d.cursor < end) {
        const pq_tq = d.src[d.cursor];
        d.cursor += 1;

        const precision = pq_tq >> Nibble.shift;
        const table_id = pq_tq & Nibble.mask;
        // 16-bit tables are legal JPEG but not baseline, and there are only
        // two table slots.
        if (table_id >= Limit.quant_tables or precision != 0) return Error.Unsupported;
        if (d.cursor + spec.Block.size > end) return Error.Protocol;

        for (0..spec.Block.size) |i| {
            d.quant[table_id][spec.zigzag[i]] = d.src[d.cursor];
            d.cursor += 1;
        }
    }
}

/// Read one table out of a DHT segment and derive its codes.
fn parseDhtOne(d: *Ctx, end: usize) Error!void {
    const tc_th = d.src[d.cursor];
    d.cursor += 1;

    const class = tc_th >> Nibble.shift;
    const table_id = tc_th & Nibble.mask;
    if (class >= Limit.huff_classes or table_id >= Limit.huff_ids) return Error.Unsupported;
    if (d.cursor + spec.Huff.lengths > end) return Error.Protocol;

    const table = if (class == 0) &d.dc_tables[table_id] else &d.ac_tables[table_id];

    var total: u16 = 0;
    for (0..spec.Huff.lengths) |i| {
        table.bits[i] = d.src[d.cursor];
        d.cursor += 1;
        total += table.bits[i];
    }
    if (total > spec.Huff.max_symbols) return Error.Protocol;
    if (d.cursor + total > end) return Error.Protocol;

    for (0..total) |i| {
        table.vals[i] = d.src[d.cursor];
        d.cursor += 1;
    }
    table.build();
}

/// Read a DHT segment, which may carry several tables back to back.
pub fn parseDht(d: *Ctx) Error!void {
    const len = try d.segmentLen(Field.length_bytes);
    const end = d.cursor + len;
    d.cursor += Field.length_bytes;

    while (d.cursor < end) try parseDhtOne(d, end);
}

/// Read the per-component entries of a frame header and record the largest
/// sampling factors, which set the MCU geometry.
fn parseSof0Components(d: *Ctx, cursor: *usize) void {
    d.hmax = 0;
    d.vmax = 0;

    for (0..d.ncomp) |i| {
        const comp = &d.comps[i];
        comp.id = d.src[cursor.*];
        cursor.* += 1;

        const hv = d.src[cursor.*];
        cursor.* += 1;
        comp.h = hv >> Nibble.shift;
        comp.v = hv & Nibble.mask;

        comp.quant_id = d.src[cursor.*];
        cursor.* += 1;

        if (comp.h > d.hmax) d.hmax = comp.h;
        if (comp.v > d.vmax) d.vmax = comp.v;
    }
}

/// Accept only the two chroma layouts the scan loop can upsample: 4:4:4, and
/// 4:2:0 with both chroma planes at 1x1.
fn checkChromaLayout(d: *const Ctx) Error!void {
    if (d.ncomp != 3) return;

    const is_444 = d.hmax == 1 and d.vmax == 1;
    const is_420 = d.hmax == 2 and d.vmax == 2 and
        d.comps[1].h == 1 and d.comps[1].v == 1 and
        d.comps[2].h == 1 and d.comps[2].v == 1;

    if (!is_444 and !is_420) return Error.Unsupported;
}

/// Read a baseline frame header: 8-bit samples, one or three components.
pub fn parseSof0(d: *Ctx) Error!void {
    const len = try d.segmentLen(Field.sof0_min);

    var cursor = d.cursor + Field.length_bytes;
    const precision = d.src[cursor];
    cursor += 1;
    if (precision != Field.precision) return Error.Unsupported;

    d.height = (@as(u16, d.src[cursor]) << 8) | d.src[cursor + 1];
    cursor += 2;
    d.width = (@as(u16, d.src[cursor]) << 8) | d.src[cursor + 1];
    cursor += 2;

    d.ncomp = d.src[cursor];
    cursor += 1;
    if (d.ncomp != 1 and d.ncomp != 3) return Error.Unsupported;

    const needed = (@as(u32, d.ncomp) * Field.sof0_per_comp) + @as(u32, @intCast(cursor - d.cursor));
    if (needed > len) return Error.Protocol;

    parseSof0Components(d, &cursor);
    d.cursor += len;
    try checkChromaLayout(d);
}

/// Read a scan header: the Huffman selectors each component decodes with.
pub fn parseSos(d: *Ctx) Error!void {
    const len = try d.segmentLen(Field.sos_min);

    var cursor = d.cursor + Field.length_bytes;
    const ns = d.src[cursor];
    cursor += 1;
    // A scan covering fewer components than the frame is progressive-shaped.
    if (ns != d.ncomp) return Error.Unsupported;

    for (0..ns) |i| {
        const selector = d.src[cursor];
        cursor += 1;
        const tdta = d.src[cursor];
        cursor += 1;

        // Match the scan entry to its frame component; fall back to position
        // when no id matches, as the C did.
        var index = i;
        for (0..d.ncomp) |j| {
            if (d.comps[j].id == selector) {
                index = j;
                break;
            }
        }

        const comp = &d.comps[index];
        comp.dc_id = tdta >> Nibble.shift;
        comp.ac_id = tdta & Nibble.mask;
        if (comp.dc_id >= Limit.huff_ids or comp.ac_id >= Limit.huff_ids) {
            return Error.Unsupported;
        }
    }

    d.cursor += len;
}
