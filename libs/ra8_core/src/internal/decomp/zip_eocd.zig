//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The bounded classic-ZIP end-of-central-directory preflight: reject an
//! over-cap archive from its EOCD before a format allocator can obscure
//! the cause with a generic allocation failure.
//!
//! Only the classic comment window is scanned, in fixed chunks, walking
//! backward from the end. A signature hit that fails re-read or
//! comment-length validation is a collision in the scan window rather than
//! the true record, so the scan continues past it.

const std = @import("std");

/// Fixed classic-ZIP EOCD geometry.
pub const geometry = struct {
    /// EOCD bytes before its comment.
    pub const eocd_bytes: usize = 22;
    /// Maximum classic ZIP comment bytes.
    pub const comment_max: u64 = 65535;
    /// Candidate offsets inspected per read.
    pub const scan_chunk: u64 = 512;
    /// Total-entry field offset within the EOCD.
    pub const entries_offset: usize = 10;
    /// Comment-length field offset within the EOCD.
    pub const comment_offset: usize = 20;
};

/// The EOCD signature: "PK\x05\x06".
pub const signature = [_]u8{ 0x50, 0x4B, 0x05, 0x06 };

/// A positioned read over an immutable container. Mirrors
/// `ra8_decomp_read_fn`, and reports a short read by returning fewer bytes,
/// which leaves validation to the format decoder. The context is opaque
/// here so this unit never sees the C callback shape.
pub const Reader = struct {
    ctx: ?*anyopaque,
    read: *const fn (ctx: ?*anyopaque, dst: []u8, offset: u64) usize,

    fn exact(self: Reader, dst: []u8, offset: u64) bool {
        return self.read(self.ctx, dst, offset) == dst.len;
    }
};

/// What the preflight concluded.
pub const Verdict = enum {
    /// No verified over-cap record; the ZIP decoder stays authoritative.
    inconclusive,
    /// A verified EOCD declares more entries than the policy admits.
    over_entry_cap,
};

fn u16le(bytes: []const u8) u16 {
    return std.mem.readInt(u16, bytes[0..2], .little);
}

/// Search one loaded window backward for a verified EOCD record.
///
/// `window` holds the candidate bytes plus the signature tail, `start` is
/// the archive offset of `window[0]`, and `count` is how many leading
/// candidate positions to test. Returns null when this window holds no
/// verified record, which means the caller should scan an earlier one.
fn scanWindow(
    reader: Reader,
    window: []const u8,
    start: u64,
    count: usize,
    archive_size: u64,
    max_entries: u32,
) ?Verdict {
    var record: [geometry.eocd_bytes]u8 = undefined;
    var i = count;
    while (i > 0) {
        i -= 1;
        if (!std.mem.startsWith(u8, window[i..], &signature)) continue;

        const position = start + i;
        if (!reader.exact(&record, position)) continue;

        const comment = u16le(record[geometry.comment_offset..]);
        if (position + record.len + comment != archive_size) continue;

        const entries = u16le(record[geometry.entries_offset..]);
        return if (entries > max_entries) .over_entry_cap else .inconclusive;
    }
    return null;
}

/// Walk the comment window backward until a verified record is found.
///
/// `scratch` is the caller's scan buffer; it must hold one chunk plus the
/// signature tail. A short read ends the scan inconclusively rather than
/// failing: read errors do not replace the decoder's final verdict.
pub fn preflight(
    reader: Reader,
    archive_size: u64,
    scratch: []u8,
    max_entries: u32,
) Verdict {
    if (archive_size < geometry.eocd_bytes) return .inconclusive;

    const tail = signature.len - 1;
    const candidates = archive_size - geometry.eocd_bytes + 1;
    const window = geometry.comment_max + 1;
    const lower = if (candidates > window) candidates - window else 0;

    var end = candidates;
    while (end > lower) {
        const start = if (end - lower > geometry.scan_chunk) end - geometry.scan_chunk else lower;
        const count: usize = @intCast(end - start);
        if (!reader.exact(scratch[0 .. count + tail], start)) return .inconclusive;
        if (scanWindow(reader, scratch, start, count, archive_size, max_entries)) |verdict| {
            return verdict;
        }
        end = start;
    }
    return .inconclusive;
}

/// Bytes a caller's scan buffer needs.
pub const scratch_bytes: usize = @as(usize, @intCast(geometry.scan_chunk)) + signature.len - 1;
