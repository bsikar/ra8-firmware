//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_imgdec_dims`: what a container says its geometry is, read from the
//! leading bytes without decoding anything.
//!
//! Four of the five containers keep their size at a fixed offset and get a
//! reader apiece below. JPEG is the exception: its geometry sits behind a
//! marker walk of unbounded length, so `jpeg` is the only reader here that
//! loops.

const vocab = @import("vocab.zig");
const abi = @import("abi.zig");
const sniff_mod = @import("sniff.zig");

const Fault = vocab.Fault;
const Format = vocab.Format;
const Geom = abi.Geom;

const Png = struct {
    const type_off: usize = 12;
    const width_off: usize = 16;
    const min_bytes: usize = 24;
};

const Gif = struct {
    const width_off: usize = 6;
    const min_bytes: usize = 10;
};

const Bmp = struct {
    const dibsize_off: usize = 14;
    const dims_off: usize = 18;
    /// The DIB size that means 16-bit dimensions.
    const core_size: u32 = 12;
    const core_bytes: usize = 22;
    const info_bytes: usize = 26;
    const core_hgt_off: usize = 20;
    const info_hgt_off: usize = 22;
};

const Webp = struct {
    const chunk_off: usize = 12;
    const payload_off: usize = 20;
    const lossy_sz_off: usize = 6;
    const lossy_bytes: usize = 10;
    const lossless_sig: u8 = 0x2F;
    const lossless_len: usize = 5;
    const ext_wid_off: usize = 4;
    const ext_hgt_off: usize = 7;
    const ext_bytes: usize = 10;
    /// Fourteen bits, the VP8 dimension width.
    const mask_14: u32 = 0x3FFF;
    const shift_14: u5 = 14;
};

const Jpeg = struct {
    /// Marker prefix, and legal as fill before one.
    const pad: u8 = 0xFF;
    const sof_first: u8 = 0xC0;
    const sof_last: u8 = 0xCF;
    /// Huffman tables, reserved, and arithmetic conditioning all sit inside the
    /// SOFn range without being frame headers.
    const dht: u8 = 0xC4;
    const jpg: u8 = 0xC8;
    const dac: u8 = 0xCC;
    const rst_first: u8 = 0xD0;
    const rst_last: u8 = 0xD7;
    const soi: u8 = 0xD8;
    const eoi: u8 = 0xD9;
    const sos: u8 = 0xDA;
    const tem: u8 = 0x01;
    const seg_min: u32 = 2;
    const sof_hgt: usize = 3;
    const sof_min: usize = 7;
};

fn be16(bytes: []const u8, offset: usize) u32 {
    return (@as(u32, bytes[offset]) << 8) | @as(u32, bytes[offset + 1]);
}

fn be32(bytes: []const u8, offset: usize) u32 {
    return (@as(u32, bytes[offset]) << 24) | (@as(u32, bytes[offset + 1]) << 16) |
        (@as(u32, bytes[offset + 2]) << 8) | @as(u32, bytes[offset + 3]);
}

fn le16(bytes: []const u8, offset: usize) u32 {
    return @as(u32, bytes[offset]) | (@as(u32, bytes[offset + 1]) << 8);
}

fn le24(bytes: []const u8, offset: usize) u32 {
    return le16(bytes, offset) | (@as(u32, bytes[offset + 2]) << 16);
}

fn le32(bytes: []const u8, offset: usize) u32 {
    return le24(bytes, offset) | (@as(u32, bytes[offset + 3]) << 24);
}

/// A BMP height is signed: negative means top-down, and the magnitude is the
/// height either way.
fn abs32(raw: u32) u32 {
    const signed: i32 = @bitCast(raw);
    if (signed < 0) return @intCast(-@as(i64, signed));
    return raw;
}

fn fourcc(bytes: []const u8, offset: usize, tag: []const u8) bool {
    const end = offset + tag.len;
    if (bytes.len < end) return false;
    for (bytes[offset..end], tag) |got, expected| {
        if (got != expected) return false;
    }
    return true;
}

fn png(bytes: []const u8) Fault!Geom {
    if (bytes.len < Png.min_bytes) return Fault.NotSupported;
    if (!fourcc(bytes, Png.type_off, "IHDR")) return Fault.NotSupported;
    return .{
        .format = Format.png,
        .width_px = be32(bytes, Png.width_off),
        .height_px = be32(bytes, Png.width_off + 4),
    };
}

fn gif(bytes: []const u8) Fault!Geom {
    if (bytes.len < Gif.min_bytes) return Fault.NotSupported;
    return .{
        .format = Format.gif,
        .width_px = le16(bytes, Gif.width_off),
        .height_px = le16(bytes, Gif.width_off + 2),
    };
}

fn bmp(bytes: []const u8) Fault!Geom {
    if (bytes.len < Bmp.dibsize_off + 4) return Fault.NotSupported;

    if (le32(bytes, Bmp.dibsize_off) == Bmp.core_size) {
        if (bytes.len < Bmp.core_bytes) return Fault.NotSupported;
        return .{
            .format = Format.bmp,
            .width_px = le16(bytes, Bmp.dims_off),
            .height_px = le16(bytes, Bmp.core_hgt_off),
        };
    }

    if (bytes.len < Bmp.info_bytes) return Fault.NotSupported;
    return .{
        .format = Format.bmp,
        .width_px = abs32(le32(bytes, Bmp.dims_off)),
        .height_px = abs32(le32(bytes, Bmp.info_hgt_off)),
    };
}

fn webp(bytes: []const u8) Fault!Geom {
    const payload = Webp.payload_off;

    if (fourcc(bytes, Webp.chunk_off, "VP8 ")) {
        if (bytes.len < payload + Webp.lossy_bytes) return Fault.NotSupported;
        const at = payload + Webp.lossy_sz_off;
        return .{
            .format = Format.webp,
            .width_px = le16(bytes, at) & Webp.mask_14,
            .height_px = le16(bytes, at + 2) & Webp.mask_14,
        };
    }

    if (fourcc(bytes, Webp.chunk_off, "VP8L")) {
        if (bytes.len < payload + Webp.lossless_len) return Fault.NotSupported;
        if (bytes[payload] != Webp.lossless_sig) return Fault.NotSupported;
        const packed_dims = le32(bytes, payload + 1);
        return .{
            .format = Format.webp,
            .width_px = (packed_dims & Webp.mask_14) + 1,
            .height_px = ((packed_dims >> Webp.shift_14) & Webp.mask_14) + 1,
        };
    }

    if (fourcc(bytes, Webp.chunk_off, "VP8X")) {
        if (bytes.len < payload + Webp.ext_bytes) return Fault.NotSupported;
        return .{
            .format = Format.webp,
            .width_px = le24(bytes, payload + Webp.ext_wid_off) + 1,
            .height_px = le24(bytes, payload + Webp.ext_hgt_off) + 1,
        };
    }

    // Recognised container, first chunk is none of the three VP8 flavours.
    return Fault.NotSupported;
}

/// The SOFn block, minus the three markers inside its range that are not frame
/// headers.
fn isSof(marker: u8) bool {
    if (marker < Jpeg.sof_first or marker > Jpeg.sof_last) return false;
    return marker != Jpeg.dht and marker != Jpeg.jpg and marker != Jpeg.dac;
}

/// Markers that carry no length field and so no payload to skip.
fn isStandalone(marker: u8) bool {
    const restart = marker >= Jpeg.rst_first and marker <= Jpeg.rst_last;
    return restart or marker == Jpeg.soi or marker == Jpeg.eoi or marker == Jpeg.tem;
}

/// Walk markers from just past the SOI to the first frame header.
fn jpeg(bytes: []const u8) Fault!Geom {
    var at: usize = 2;

    while (bytes.len >= at + 2) {
        // Desynchronised: this is not a marker boundary.
        if (bytes[at] != Jpeg.pad) return Fault.NotSupported;

        var marker = bytes[at + 1];
        at += 2;

        // 0xFF is legal fill before a marker; skip any run of it.
        while (marker == Jpeg.pad and bytes.len >= at + 1) {
            marker = bytes[at];
            at += 1;
        }

        // Entropy data reached with no frame header.
        if (marker == Jpeg.sos) return Fault.NotSupported;

        if (isStandalone(marker)) {
            // The image ended with no frame header.
            if (marker == Jpeg.eoi) return Fault.NotSupported;
            continue;
        }

        if (bytes.len < at + 2) return Fault.NotSupported;
        const seg_len = be16(bytes, at);
        if (seg_len < Jpeg.seg_min) return Fault.NotSupported;

        if (isSof(marker)) {
            if (seg_len < Jpeg.sof_min or bytes.len < at + Jpeg.sof_min) {
                return Fault.NotSupported;
            }
            return .{
                .format = Format.jpeg,
                .height_px = be16(bytes, at + Jpeg.sof_hgt),
                .width_px = be16(bytes, at + Jpeg.sof_hgt + 2),
            };
        }

        at += seg_len;
    }

    // Ran out of bytes before any frame header.
    return Fault.NotSupported;
}

/// A container's declared geometry, sniffed then read at that container's own
/// offsets.
///
/// `Fault.InvalidSize` covers both an empty buffer and a declared dimension of
/// 0 or over `Limits.dim_max`; `Fault.NotFound` means no signature matched;
/// `Fault.NotSupported` means the container was recognised but its geometry is
/// not readable here.
pub fn dims(bytes: []const u8) Fault!Geom {
    const found = try sniff_mod.sniff(bytes);

    const geom = switch (found) {
        Format.png => try png(bytes),
        Format.jpeg => try jpeg(bytes),
        Format.webp => try webp(bytes),
        Format.gif => try gif(bytes),
        Format.bmp => try bmp(bytes),
        // No signature keys a TGA geometry read.
        else => return Fault.NotSupported,
    };

    if (geom.width_px == 0 or geom.height_px == 0) return Fault.InvalidSize;
    if (geom.width_px > vocab.Limits.dim_max or geom.height_px > vocab.Limits.dim_max) {
        return Fault.InvalidSize;
    }
    return geom;
}
