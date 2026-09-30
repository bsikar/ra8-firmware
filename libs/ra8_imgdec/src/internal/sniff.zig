//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_imgdec_sniff`: name the container a buffer opens with, from fixed
//! signatures in its leading bytes and nothing else.
//!
//! TGA has no signature at all, so it is never answered here; a caller holding
//! one declares `Format.tga` in the request instead.

const vocab = @import("vocab.zig");

const Fault = vocab.Fault;
const Format = vocab.Format;

/// Where a signature sits inside the leading bytes.
const Off = struct {
    const webp_riff: usize = 0;
    const webp_form: usize = 8;
    const gif_ver: usize = 3;
};

const png_signature = [_]u8{ 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
const jpeg_signature = [_]u8{ 0xFF, 0xD8, 0xFF };

/// True when `bytes` carries `want` at `offset`, bounds included.
fn matches(bytes: []const u8, offset: usize, want: []const u8) bool {
    const end = offset + want.len;
    if (bytes.len < end) return false;
    for (bytes[offset..end], want) |got, expected| {
        if (got != expected) return false;
    }
    return true;
}

fn isWebp(bytes: []const u8) bool {
    return matches(bytes, Off.webp_riff, "RIFF") and matches(bytes, Off.webp_form, "WEBP");
}

fn isGif(bytes: []const u8) bool {
    if (!matches(bytes, 0, "GIF")) return false;
    return matches(bytes, Off.gif_ver, "87a") or matches(bytes, Off.gif_ver, "89a");
}

/// The container `bytes` opens with.
///
/// Returns `Fault.InvalidSize` for an empty buffer and `Fault.NotFound` when no
/// signature matches, including a buffer too short to carry one.
pub fn sniff(bytes: []const u8) Fault!u32 {
    if (bytes.len == 0) return Fault.InvalidSize;

    if (matches(bytes, 0, &png_signature)) return Format.png;
    if (matches(bytes, 0, &jpeg_signature)) return Format.jpeg;
    if (isWebp(bytes)) return Format.webp;
    if (isGif(bytes)) return Format.gif;
    if (matches(bytes, 0, "BM")) return Format.bmp;
    return Fault.NotFound;
}
