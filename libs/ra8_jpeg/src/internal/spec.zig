//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Baseline JPEG constants and the zig-zag permutation.
//!
//! Every value the encoder needs from ITU-T T.81 lives here, grouped by what
//! it describes rather than flattened into one `k_ra8_jpeg_*` enum. Nothing in
//! this file computes; it is the spec, transcribed.

/// Segment marker codes, T.81 sec B.1.1.3 "Marker assignments".
pub const Marker = struct {
    pub const soi: u16 = 0xFFD8;
    pub const eoi: u16 = 0xFFD9;
    pub const sos: u16 = 0xFFDA;
    pub const dqt: u16 = 0xFFDB;
    pub const dht: u16 = 0xFFC4;
    pub const sof0: u16 = 0xFFC0;
    pub const app0: u16 = 0xFFE0;

    /// An entropy byte equal to this is followed by a stuffed `0x00`,
    /// T.81 sec F.1.2.3.
    pub const stuff_trigger: u8 = 0xFF;
};

/// The 8x8 DCT block, T.81 sec A.2.
pub const Block = struct {
    pub const dim: u16 = 8;
    pub const size: usize = 64;

    /// Samples are centred on zero before the forward DCT, T.81 sec A.3.1.
    pub const level_offset: i32 = 128;
    pub const sample_max: u32 = 255;
};

/// The 4:2:0 minimum coded unit: 16x16 luma pixels, T.81 sec A.2.3.
pub const Mcu = struct {
    pub const dim: u16 = 16;
    pub const align_mask: u32 = 15;

    /// Round `n` up to a whole MCU. Returned wide so the caller can reject an
    /// over-wide image before the value is narrowed, rather than after.
    pub fn alignUp(n: u16) u32 {
        return (@as(u32, n) + align_mask) & ~align_mask;
    }
};

/// Huffman coding shape, T.81 sec B.2.4.2 and Annex C.
pub const Huff = struct {
    /// A BITS list always has 16 slots, one per code length 1..16.
    pub const lengths: usize = 16;
    /// A symbol is one byte, so a table is indexed 0..255.
    pub const max_symbols: usize = 256;
    /// Zero-run-length symbol, T.81 sec F.1.2.2.
    pub const zrl: u8 = 0xF0;
    /// End-of-block symbol.
    pub const eob: u8 = 0x00;
    /// A run of this many zeroes is emitted as one ZRL.
    pub const zrl_run: u8 = 16;
    /// Widest run that still fits the RRRR nibble alongside SSSS.
    pub const max_run: u8 = 15;
    /// Widest magnitude category that still fits the SSSS nibble.
    pub const max_magnitude: u8 = 15;
};

/// Header segment payloads this encoder emits, fixed because the output is
/// always 4:2:0 YCbCr baseline with two quantisation and four Huffman tables.
pub const Segment = struct {
    pub const app0_len: u16 = 16;
    pub const sof0_len: u16 = 17;
    pub const sos_len: u16 = 12;
    pub const precision: u8 = 8;
    pub const components: u8 = 3;
    pub const spectral_end: u8 = 63;

    /// SOF0 component sampling bytes: luma 2x2, chroma 1x1.
    pub const sampling_luma: u8 = 0x22;
    pub const sampling_chroma: u8 = 0x11;

    /// DHT table-class/identifier bytes (TcTh).
    pub const dht_dc_luma: u8 = 0x00;
    pub const dht_ac_luma: u8 = 0x10;
    pub const dht_dc_chroma: u8 = 0x01;
    pub const dht_ac_chroma: u8 = 0x11;

    /// SOS table selector bytes (TdTa).
    pub const sos_luma: u8 = 0x00;
    pub const sos_chroma: u8 = 0x11;
};

/// Bounds the encoder enforces on its own inputs and working set.
pub const Limits = struct {
    /// Widest image the static strip buffers cover, in padded pixels.
    pub const max_width: u32 = 1024;
    /// YCbCr, so three DC predictors and three SOF0 component records.
    pub const components: usize = 3;
    /// Source pixels are packed RGB888.
    pub const rgb_channels: usize = 3;
    pub const quality_min: u8 = 1;
    pub const quality_max: u8 = 100;
};

/// Zig-zag scan order, T.81 Figure A.6: `zigzag[i]` is the raster index of
/// the i-th coefficient in scan order.
pub const zigzag: [Block.size]u8 = .{
    0,  1,  8,  16, 9,  2,  3,  10,
    17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};
