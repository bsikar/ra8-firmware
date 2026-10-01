//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Decoder parse state for the baseline decoder.
//!
//! Everything a marker parser fills in and the scan loop then reads: the frame
//! geometry, the per-component selectors, and the dequantisation and Huffman
//! tables. Held by value, so the whole-buffer and striped drivers each own one
//! and neither reaches into the other's.

const huffdec = @import("huffdec");
const spec = @import("spec");

/// Table and component counts this decoder accepts. Baseline JFIF never needs
/// more, and the fixed sizes keep the context allocation static.
pub const Limit = struct {
    /// One luma plus one chroma quantisation table.
    pub const quant_tables: usize = 2;
    /// Luma plus chroma Huffman table ids, per class.
    pub const huff_ids: usize = 2;
    /// DC and AC.
    pub const huff_classes: u8 = 2;
    /// Grayscale or Y'CbCr.
    pub const max_components: usize = spec.Limits.components;
    /// A 4:2:0 MCU is 16x16 luma pixels, the largest this decoder emits.
    pub const mcu_max_dim: usize = 16;
};

/// Bit positions shared by every packed selector byte in the marker stream:
/// a high nibble and a low nibble.
pub const Nibble = struct {
    pub const shift: u3 = 4;
    pub const mask: u8 = 0x0F;
};

/// What a parse can conclude. The ABI membrane maps these onto `ra8_err_t`.
pub const Error = error{
    /// Malformed stream: a bad marker, or a length that walks off the buffer.
    Protocol,
    /// Well-formed but outside baseline: 16-bit tables, progressive frames,
    /// a chroma layout other than 4:4:4 or 4:2:0.
    Unsupported,
    /// The caller's output buffer is too small for the decoded frame.
    InvalidSize,
};

/// One component's entry in the frame header and the scan header.
pub const Component = struct {
    id: u8 = 0,
    /// Horizontal and vertical sampling factors.
    h: u8 = 0,
    v: u8 = 0,
    /// Quantisation table selector.
    quant_id: u8 = 0,
    /// DC and AC Huffman table selectors, set by SOS.
    dc_id: u8 = 0,
    ac_id: u8 = 0,
    /// Running DC predictor, reset at the start of every scan.
    dc_pred: i32 = 0,
};

/// Parse state for one decode.
pub const Ctx = struct {
    /// The bytes the parsers read from, and how far in they are.
    src: []const u8 = &.{},
    cursor: usize = 0,

    width: u16 = 0,
    height: u16 = 0,
    ncomp: u8 = 0,
    /// Largest sampling factor across components; sets the MCU size.
    hmax: u8 = 0,
    vmax: u8 = 0,

    comps: [Limit.max_components]Component = .{Component{}} ** Limit.max_components,

    quant: [Limit.quant_tables][spec.Block.size]u16 =
        .{[_]u16{0} ** spec.Block.size} ** Limit.quant_tables,
    dc_tables: [Limit.huff_ids]huffdec.Table = undefined,
    ac_tables: [Limit.huff_ids]huffdec.Table = undefined,

    /// Zero the context the way the C `memset` did, so a table left unset by
    /// the stream decodes as zeros rather than as the previous image's.
    pub fn reset(self: *Ctx) void {
        self.* = .{};
        for (&self.dc_tables) |*table| table.* = .{
            .bits = .{0} ** spec.Huff.lengths,
            .vals = .{0} ** spec.Huff.max_symbols,
            .huffcode = .{0} ** spec.Huff.max_symbols,
            .huffsize = .{0} ** spec.Huff.max_symbols,
            .mincode = .{0} ** spec.Huff.lengths,
            .maxcode = .{0} ** spec.Huff.lengths,
            .valptr = .{0} ** spec.Huff.lengths,
            .total = 0,
        };
        self.ac_tables = self.dc_tables;
    }

    /// Bytes still unread ahead of the cursor.
    pub fn remaining(self: *const Ctx) usize {
        return self.src.len - self.cursor;
    }

    /// The big-endian short at the cursor, or null when it is not there.
    pub fn peekBe16(self: *const Ctx) ?u16 {
        if (self.cursor + 2 > self.src.len) return null;
        return (@as(u16, self.src[self.cursor]) << 8) | self.src[self.cursor + 1];
    }

    /// The segment length at the cursor, validated against the buffer. Every
    /// marker parser starts with this, so the bound is checked once.
    pub fn segmentLen(self: *const Ctx, minimum: u16) Error!u16 {
        const len = self.peekBe16() orelse return Error.Protocol;
        if (len < minimum or len > self.remaining()) return Error.Protocol;
        return len;
    }

    /// The DC table a component decodes with.
    pub fn dcTable(self: *const Ctx, ci: usize) *const huffdec.Table {
        return &self.dc_tables[self.comps[ci].dc_id];
    }

    /// The AC table a component decodes with.
    pub fn acTable(self: *const Ctx, ci: usize) *const huffdec.Table {
        return &self.ac_tables[self.comps[ci].ac_id];
    }
};
