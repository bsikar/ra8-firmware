//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure runtime logic for `ra8_cache_store`: the on-flash entry-header record,
//! the CRC-32 that seals it, the free-run allocator over the live index and the
//! sector arithmetic the streaming reader walks.
//!
//! Nothing here touches LevelX, the staging buffer or the C ABI. Every function
//! is total over its arguments, which is what lets the allocator and the read
//! splitter be tested without a medium.

const std = @import("std");

/// LevelX logical-sector size (`k_ra8_cache_store_sector_bytes`).
pub const sector_bytes: u32 = 512;
/// Longest run the 16-bit `sector_count` field carries (`k_ra8_cs_max_run`).
pub const max_run: u32 = 0xFFFF;
/// Entry-header tag 'R','C','S','E' (`k_ra8_cs_entry_magic`).
pub const entry_magic: u32 = 0x52435345;
/// Reflected CRC-32/ISO-HDLC polynomial (`k_ra8_cs_crc32_poly`).
pub const crc32_poly: u32 = 0xEDB88320;
/// CRC-32 pre/post conditioning (`k_ra8_cs_crc32_seed`).
pub const crc32_seed: u32 = 0xFFFFFFFF;

/// Empty slot (`k_ra8_cache_store_flag_none`).
pub const flag_none: u8 = 0;
/// Slot holds a live entry (`k_ra8_cache_store_flag_in_use`).
pub const flag_in_use: u8 = 1 << 0;
/// Never-evict marker (`k_ra8_cache_store_flag_pinned`).
pub const flag_pinned: u8 = 1 << 1;

/// One in-RAM index slot (`ra8_cache_store_entry_t`), pinned at 16 bytes.
pub const Entry = extern struct {
    key: u32 = 0,
    start_sector: u32 = 0,
    byte_len: u32 = 0,
    sector_count: u16 = 0,
    flags: u8 = 0,
    reserved: u8 = 0,
};

/// On-flash entry header (`ra8_cs_entry_hdr_t`), written last in a put so its
/// presence certifies a complete run.
pub const EntryHeader = extern struct {
    magic: u32 = entry_magic,
    seq: u32 = 0,
    key: u32 = 0,
    byte_len: u32 = 0,
    start_sector: u32 = 0,
    sector_count: u16 = 0,
    flags: u16 = 0,
    hdr_crc: u32 = 0,
};

comptime {
    // These are the C ABI, not implementation detail: the header is memcpy'd
    // into a sector and read back by the mount scanner in the C TU.
    std.debug.assert(@sizeOf(Entry) == 16);
    std.debug.assert(@offsetOf(Entry, "sector_count") == 12);
    std.debug.assert(@offsetOf(Entry, "flags") == 14);
    std.debug.assert(@offsetOf(Entry, "reserved") == 15);
    std.debug.assert(@sizeOf(EntryHeader) == 28);
    std.debug.assert(@offsetOf(EntryHeader, "start_sector") == 16);
    std.debug.assert(@offsetOf(EntryHeader, "sector_count") == 20);
    std.debug.assert(@offsetOf(EntryHeader, "flags") == 22);
    std.debug.assert(@offsetOf(EntryHeader, "hdr_crc") == 24);
    std.debug.assert(@sizeOf(EntryHeader) <= sector_bytes);
}

/// Bytes of an entry header covered by its CRC (everything before `hdr_crc`).
pub const header_crc_span: u32 = @sizeOf(EntryHeader) - @sizeOf(u32);

/// Fold a block into a seeded and finalised CRC-32/ISO-HDLC value bit by bit,
/// exactly as `priv_cache_store_crc32` does (no lookup table, so no .rodata).
pub fn crc32(data: []const u8) u32 {
    var crc: u32 = crc32_seed;
    for (data) |byte| {
        crc ^= byte;
        var bit: u8 = 0;
        while (bit < 8) : (bit += 1) {
            const lsb_set = (crc & 1) != 0;
            crc >>= 1;
            if (lsb_set) crc ^= crc32_poly;
        }
    }
    return crc ^ crc32_seed;
}

/// Seal a header with the CRC over its preceding fields.
pub fn sealHeader(header: *EntryHeader) void {
    header.hdr_crc = crc32(std.mem.asBytes(header)[0..header_crc_span]);
}

/// True when a slot carries a live entry.
pub fn inUse(entry: Entry) bool {
    return (entry.flags & flag_in_use) != 0;
}

/// True when a slot is marked never-evict.
pub fn isPinned(entry: Entry) bool {
    return (entry.flags & flag_pinned) != 0;
}

/// Count the in-use slots (`internal_index_used`).
pub fn indexUsed(entries: []const Entry) u16 {
    var used: u16 = 0;
    for (entries) |entry| {
        if (inUse(entry)) used += 1;
    }
    return used;
}

/// Slot holding `key`, or null when absent (`priv_cache_store_index_find`).
pub fn findKey(entries: []const Entry, key: u32) ?u16 {
    for (entries, 0..) |entry, slot| {
        if (!inUse(entry)) continue;
        if (entry.key == key) return @intCast(slot);
    }
    return null;
}

/// True when `[start, start + count)` overlaps no live run (`internal_run_free`).
/// Widened to 64-bit so a nonsense length cannot wrap past the comparison the
/// way the C's `uint32_t` addition could.
pub fn runFree(entries: []const Entry, start: u32, count: u32) bool {
    const end: u64 = @as(u64, start) + count;
    for (entries) |entry| {
        if (!inUse(entry)) continue;
        const entry_start: u64 = entry.start_sector;
        const entry_end: u64 = entry_start + entry.sector_count;
        if (start < entry_end and entry_start < end) return false;
    }
    return true;
}

/// First-fit a free run of `count` sectors in `[log_start, logical_sectors)`
/// (`internal_alloc_run`); null when no such run exists.
pub fn allocRun(entries: []const Entry, log_start: u32, logical_sectors: u32, count: u32) ?u32 {
    var start: u32 = log_start;
    while (@as(u64, start) + count <= logical_sectors) : (start += 1) {
        if (runFree(entries, start, count)) return start;
    }
    return null;
}

/// Run length for a payload of `len` bytes: the header sector plus the payload
/// sectors. Null when the run would overflow the header's 16-bit field.
pub fn runLength(len: u32) ?u32 {
    const count: u32 = 1 + ((len + (sector_bytes - 1)) / sector_bytes);
    if (count > max_run) return null;
    return count;
}

/// Bytes of payload to stage into one sector starting at `offset`.
pub fn payloadChunk(len: u32, offset: u32) u32 {
    if (offset >= len) return 0;
    const remain = len - offset;
    return if (remain < sector_bytes) remain else sector_bytes;
}

/// Where one read lands: the logical sector, the byte offset inside it and how
/// many bytes this pass may copy.
pub const ReadSlice = struct {
    sector: u32,
    offset: u32,
    chunk: u32,
};

/// Split a payload-relative read into a single-sector copy (`internal_read_at`).
pub fn sliceAt(data_start: u32, byte_pos: u64, max: u32) ReadSlice {
    const sector: u32 = data_start + @as(u32, @intCast(byte_pos / sector_bytes));
    const offset: u32 = @intCast(byte_pos % sector_bytes);
    const space: u32 = sector_bytes - offset;
    return .{ .sector = sector, .offset = offset, .chunk = if (max < space) max else space };
}
