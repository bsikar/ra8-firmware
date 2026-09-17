//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the pure runtime logic: the CRC that seals an entry
//! header, the record layout, the free-run allocator over the live index and
//! the sector arithmetic the streaming reader walks. No medium and no ABI are
//! involved, so every case here is a plain function call.

const std = @import("std");
const implementation = @import("implementation");

const Entry = implementation.Entry;

fn live(key: u32, start: u32, count: u16) Entry {
    return .{
        .key = key,
        .start_sector = start,
        .byte_len = @as(u32, count - 1) * implementation.sector_bytes,
        .sector_count = count,
        .flags = implementation.flag_in_use,
    };
}

test "crc32 of an empty block is the conditioning cancelling out" {
    try std.testing.expectEqual(@as(u32, 0), implementation.crc32(&[_]u8{}));
}

test "crc32 matches the ISO-HDLC check value" {
    try std.testing.expectEqual(@as(u32, 0xCBF43926), implementation.crc32("123456789"));
}

test "crc32 is sensitive to a single flipped bit" {
    const a = implementation.crc32("cache");
    const b = implementation.crc32("cachf");
    try std.testing.expect(a != b);
}

test "crc32 is order sensitive" {
    try std.testing.expect(implementation.crc32("ab") != implementation.crc32("ba"));
}

test "entry header is 28 bytes with the C field order" {
    try std.testing.expectEqual(@as(usize, 28), @sizeOf(implementation.EntryHeader));
    try std.testing.expectEqual(@as(u32, 24), implementation.header_crc_span);
}

test "sealHeader covers every field before the crc" {
    var header = implementation.EntryHeader{
        .seq = 7,
        .key = 0xABCD1234,
        .byte_len = 900,
        .start_sector = 12,
        .sector_count = 3,
    };
    implementation.sealHeader(&header);
    const sealed = header.hdr_crc;
    try std.testing.expect(sealed != 0);

    // Re-sealing an unchanged header is stable.
    implementation.sealHeader(&header);
    try std.testing.expectEqual(sealed, header.hdr_crc);

    // A changed field changes the seal.
    header.seq = 8;
    implementation.sealHeader(&header);
    try std.testing.expect(sealed != header.hdr_crc);
}

test "sealHeader ignores the crc field's own bytes" {
    var a = implementation.EntryHeader{ .seq = 1, .key = 2, .byte_len = 3, .start_sector = 4, .sector_count = 5 };
    var b = a;
    b.hdr_crc = 0xDEADBEEF;
    implementation.sealHeader(&a);
    implementation.sealHeader(&b);
    try std.testing.expectEqual(a.hdr_crc, b.hdr_crc);
}

test "entry slot is 16 bytes" {
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(Entry));
}

test "inUse and isPinned read the documented bits" {
    var entry = Entry{};
    try std.testing.expect(!implementation.inUse(entry));
    try std.testing.expect(!implementation.isPinned(entry));
    entry.flags = implementation.flag_in_use;
    try std.testing.expect(implementation.inUse(entry));
    try std.testing.expect(!implementation.isPinned(entry));
    entry.flags |= implementation.flag_pinned;
    try std.testing.expect(implementation.isPinned(entry));
}

test "indexUsed counts only in-use slots" {
    var entries = [_]Entry{ live(1, 8, 2), .{}, live(2, 10, 3), .{} };
    try std.testing.expectEqual(@as(u16, 2), implementation.indexUsed(&entries));
}

test "indexUsed of an empty index is zero" {
    var entries = [_]Entry{ .{}, .{} };
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&entries));
}

test "findKey returns the slot of a live entry" {
    var entries = [_]Entry{ .{}, live(0x11, 8, 2), live(0x22, 10, 2) };
    try std.testing.expectEqual(@as(?u16, 2), implementation.findKey(&entries, 0x22));
    try std.testing.expectEqual(@as(?u16, 1), implementation.findKey(&entries, 0x11));
}

test "findKey skips a freed slot that still carries the key bytes" {
    var entries = [_]Entry{Entry{ .key = 0x33, .start_sector = 8, .sector_count = 2, .flags = 0 }};
    try std.testing.expectEqual(@as(?u16, null), implementation.findKey(&entries, 0x33));
}

test "findKey on an all-empty index misses" {
    var entries = [_]Entry{ .{}, .{} };
    try std.testing.expectEqual(@as(?u16, null), implementation.findKey(&entries, 9));
}

test "runFree accepts a gap before a live run" {
    var entries = [_]Entry{live(1, 10, 4)};
    try std.testing.expect(implementation.runFree(&entries, 6, 4));
}

test "runFree accepts a run that starts exactly where a live run ends" {
    var entries = [_]Entry{live(1, 10, 4)};
    try std.testing.expect(implementation.runFree(&entries, 14, 2));
}

test "runFree rejects a run overlapping the live run's tail" {
    var entries = [_]Entry{live(1, 10, 4)};
    try std.testing.expect(!implementation.runFree(&entries, 13, 2));
}

test "runFree rejects a run that contains a live run" {
    var entries = [_]Entry{live(1, 10, 2)};
    try std.testing.expect(!implementation.runFree(&entries, 8, 8));
}

test "runFree ignores freed slots" {
    var entries = [_]Entry{Entry{ .start_sector = 10, .sector_count = 4, .flags = 0 }};
    try std.testing.expect(implementation.runFree(&entries, 10, 4));
}

test "runFree does not wrap past a live run on an absurd length" {
    // The C computed `start + count` in uint32_t, so a length this large wrapped
    // the end sector below the live run and reported the candidate free. The
    // 64-bit widening keeps the overlap visible.
    var entries = [_]Entry{live(1, 10, 2)};
    try std.testing.expect(!implementation.runFree(&entries, 5, 0xFFFFFFFF));
    try std.testing.expect(implementation.runFree(&entries, 12, 0xFFFF));
}

test "allocRun first-fits from log_start" {
    var entries = [_]Entry{ .{}, .{} };
    try std.testing.expectEqual(@as(?u32, 4), implementation.allocRun(&entries, 4, 64, 3));
}

test "allocRun skips past a live run" {
    var entries = [_]Entry{live(1, 4, 3)};
    try std.testing.expectEqual(@as(?u32, 7), implementation.allocRun(&entries, 4, 64, 2));
}

test "allocRun fills a hole big enough between two live runs" {
    var entries = [_]Entry{ live(1, 4, 2), live(2, 9, 2) };
    try std.testing.expectEqual(@as(?u32, 6), implementation.allocRun(&entries, 4, 64, 3));
}

test "allocRun refuses a hole one sector too small" {
    var entries = [_]Entry{ live(1, 4, 2), live(2, 8, 2) };
    try std.testing.expectEqual(@as(?u32, 10), implementation.allocRun(&entries, 4, 64, 3));
}

test "allocRun returns null when the span cannot hold the run" {
    var entries = [_]Entry{ .{}, .{} };
    try std.testing.expectEqual(@as(?u32, null), implementation.allocRun(&entries, 60, 64, 8));
}

test "allocRun returns null when every sector is live" {
    var entries = [_]Entry{live(1, 4, 8)};
    try std.testing.expectEqual(@as(?u32, null), implementation.allocRun(&entries, 4, 12, 2));
}

test "allocRun can place a run ending on the last usable sector" {
    var entries = [_]Entry{ .{}, .{} };
    try std.testing.expectEqual(@as(?u32, 4), implementation.allocRun(&entries, 4, 8, 4));
}

test "runLength charges one header sector plus the payload sectors" {
    try std.testing.expectEqual(@as(?u32, 2), implementation.runLength(1));
    try std.testing.expectEqual(@as(?u32, 2), implementation.runLength(512));
    try std.testing.expectEqual(@as(?u32, 3), implementation.runLength(513));
    try std.testing.expectEqual(@as(?u32, 3), implementation.runLength(1024));
    try std.testing.expectEqual(@as(?u32, 4), implementation.runLength(1025));
}

test "runLength refuses a payload that would overflow sector_count" {
    const too_long: u32 = implementation.max_run * implementation.sector_bytes;
    try std.testing.expectEqual(@as(?u32, null), implementation.runLength(too_long));
}

test "runLength accepts the largest payload that still fits the field" {
    const widest: u32 = (implementation.max_run - 1) * implementation.sector_bytes;
    try std.testing.expectEqual(@as(?u32, implementation.max_run), implementation.runLength(widest));
}

test "payloadChunk saturates at one sector then tapers to the tail" {
    try std.testing.expectEqual(@as(u32, 512), implementation.payloadChunk(1200, 0));
    try std.testing.expectEqual(@as(u32, 512), implementation.payloadChunk(1200, 512));
    try std.testing.expectEqual(@as(u32, 176), implementation.payloadChunk(1200, 1024));
}

test "payloadChunk is zero once the payload is consumed" {
    try std.testing.expectEqual(@as(u32, 0), implementation.payloadChunk(512, 512));
    try std.testing.expectEqual(@as(u32, 0), implementation.payloadChunk(0, 0));
}

test "sliceAt maps a payload offset onto a sector and an in-sector offset" {
    const slice = implementation.sliceAt(9, 0, 512);
    try std.testing.expectEqual(@as(u32, 9), slice.sector);
    try std.testing.expectEqual(@as(u32, 0), slice.offset);
    try std.testing.expectEqual(@as(u32, 512), slice.chunk);
}

test "sliceAt never crosses a sector boundary" {
    const slice = implementation.sliceAt(9, 500, 512);
    try std.testing.expectEqual(@as(u32, 9), slice.sector);
    try std.testing.expectEqual(@as(u32, 500), slice.offset);
    try std.testing.expectEqual(@as(u32, 12), slice.chunk);
}

test "sliceAt advances a sector at the boundary" {
    const slice = implementation.sliceAt(9, 512, 64);
    try std.testing.expectEqual(@as(u32, 10), slice.sector);
    try std.testing.expectEqual(@as(u32, 0), slice.offset);
    try std.testing.expectEqual(@as(u32, 64), slice.chunk);
}

test "sliceAt honours a small request inside a sector" {
    const slice = implementation.sliceAt(3, 1030, 5);
    try std.testing.expectEqual(@as(u32, 5), slice.sector);
    try std.testing.expectEqual(@as(u32, 6), slice.offset);
    try std.testing.expectEqual(@as(u32, 5), slice.chunk);
}

test "sliceAt walks a whole payload in sector-bounded steps" {
    const len: u32 = 1300;
    var done: u32 = 0;
    var passes: u32 = 0;
    while (done < len) {
        const slice = implementation.sliceAt(2, done, len - done);
        try std.testing.expect(slice.chunk >= 1);
        try std.testing.expect(slice.offset + slice.chunk <= implementation.sector_bytes);
        done += slice.chunk;
        passes += 1;
        try std.testing.expect(passes <= 8);
    }
    try std.testing.expectEqual(len, done);
    try std.testing.expectEqual(@as(u32, 3), passes);
}
