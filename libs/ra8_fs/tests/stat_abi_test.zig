//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Path metadata lookup (RA8FW-1091): FAT timestamp decodes, entry-to-stat,
//! and the public `ra8_fs_stat` paths over the walker fakes.

const std = @import("std");
const fs = @import("ra8_fs");
const stat = fs.stat;
const fake = @import("fs_walker_fake.zig");
const c = fs.c;

const fat16: u8 = c.k_ra8_fs_type_fat16;
const exfat: u8 = c.k_ra8_fs_type_exfat;

/// Stage a real File+Stream+Name set for `name` in root slots 2..4, with the
/// real set checksum stamped over the three entries.
fn stageSet(name: []const u8, file_attr: u8, first: u32, dlen: u64) void {
    fake.dir[2][0] = 0x85;
    fake.dir[2][1] = 2; // Stream + Name
    fake.dir[2][4] = file_attr;
    fake.dir[3][0] = 0xC0;
    fake.dir[3][3] = @intCast(name.len);
    std.mem.writeInt(u64, fake.dir[3][8..16], dlen, .little);
    std.mem.writeInt(u32, fake.dir[3][0x14..0x18], first, .little);
    std.mem.writeInt(u64, fake.dir[3][0x18..0x20], dlen, .little);
    fake.dir[4][0] = 0xC1;
    fake.dir[4][1] = 0;
    for (name, 0..) |ch, i| fake.dir[4][2 + i * 2] = ch;
    fake.dir[4][2 + name.len * 2] = 0xFF;
    fake.dir[4][3 + name.len * 2] = 0xFF;
    _ = c.priv_exfat_set_checksum(@ptrCast(&fake.dir[2]), 96);
}

// 2026-11-02 07:15:30 packed the FAT way.
const test_date: u16 = (46 << 9) | (11 << 5) | 2; // 0x5D62
const test_time: u16 = (7 << 11) | (15 << 5) | 15; // 0x39EF
const test_packed: u32 = (@as(u32, test_date) << 16) | test_time;

test "decode_fat decodes a legal packed stamp" {
    var ts: stat.Timestamp = undefined;
    stat.stat_decode_fat(test_date, test_time, 0, false, &ts);
    try std.testing.expect(ts.valid);
    try std.testing.expect(!ts.utc_offset_valid);
    try std.testing.expectEqual(@as(u16, 2026), ts.value.year);
    try std.testing.expectEqual(@as(u8, 11), ts.value.month);
    try std.testing.expectEqual(@as(u8, 2), ts.value.day);
    try std.testing.expectEqual(@as(u8, 7), ts.value.hour);
    try std.testing.expectEqual(@as(u8, 15), ts.value.minute);
    try std.testing.expectEqual(@as(u8, 30), ts.value.second);
}

test "decode_fat folds the creation tenth byte into seconds" {
    var ts: stat.Timestamp = undefined;
    stat.stat_decode_fat(test_date, test_time, 137, true, &ts);
    try std.testing.expect(ts.valid);
    try std.testing.expectEqual(@as(u8, 31), ts.value.second);
    try std.testing.expectEqual(@as(u8, 37), ts.value.centisecond);
}

test "decode_fat rejects illegal calendar fields" {
    var ts: stat.Timestamp = undefined;
    // month 0
    stat.stat_decode_fat((46 << 9) | (0 << 5) | 2, test_time, 0, false, &ts);
    try std.testing.expect(!ts.valid);
    // month 13
    stat.stat_decode_fat((46 << 9) | (13 << 5) | 2, test_time, 0, false, &ts);
    try std.testing.expect(!ts.valid);
    // day 0
    stat.stat_decode_fat((46 << 9) | (11 << 5) | 0, test_time, 0, false, &ts);
    try std.testing.expect(!ts.valid);
    // hour 24
    stat.stat_decode_fat(test_date, (24 << 11) | (15 << 5), 0, false, &ts);
    try std.testing.expect(!ts.valid);
    // minute 60
    stat.stat_decode_fat(test_date, (7 << 11) | (60 << 5), 0, false, &ts);
    try std.testing.expect(!ts.valid);
    // second 62 (odd half-second field 31 * 2)
    stat.stat_decode_fat(test_date, (7 << 11) | (15 << 5) | 31, 0, false, &ts);
    try std.testing.expect(!ts.valid);
    // creation tenth 200
    stat.stat_decode_fat(test_date, test_time, 200, true, &ts);
    try std.testing.expect(!ts.valid);
}

test "decode_exfat applies the UTC offset only when the valid bit is set" {
    var ts: stat.Timestamp = undefined;
    // +2h: valid bit set, 8 steps of 15 minutes.
    stat.stat_decode_exfat(test_packed, 0, false, 0x80 | 8, &ts);
    try std.testing.expect(ts.valid);
    try std.testing.expect(ts.utc_offset_valid);
    try std.testing.expectEqual(@as(i16, 120), ts.value.utc_offset_min);
    // negative: steps 0x7C with the sign bit -> -4 steps -> -60 minutes.
    stat.stat_decode_exfat(test_packed, 0, false, 0x80 | 0x7C, &ts);
    try std.testing.expect(ts.utc_offset_valid);
    try std.testing.expectEqual(@as(i16, -60), ts.value.utc_offset_min);
    // valid bit clear: the zone is unknown, never fabricated as UTC+00:00.
    stat.stat_decode_exfat(test_packed, 0, false, 8, &ts);
    try std.testing.expect(ts.valid);
    try std.testing.expect(!ts.utc_offset_valid);
    // 57 steps = 855 minutes, past the UTC+14:00 span: rejected.
    stat.stat_decode_exfat(test_packed, 0, false, 0x80 | 57, &ts);
    try std.testing.expect(ts.valid);
    try std.testing.expect(!ts.utc_offset_valid);
    // 48 steps = exactly UTC+12:00: accepted.
    stat.stat_decode_exfat(test_packed, 0, false, 0x80 | 48, &ts);
    try std.testing.expect(ts.utc_offset_valid);
    try std.testing.expectEqual(@as(i16, 720), ts.value.utc_offset_min);
    // an illegal packed stamp stays invalid regardless of the zone byte.
    stat.stat_decode_exfat(0x0000_0000, 0, false, 0x80 | 8, &ts);
    try std.testing.expect(!ts.valid);
}

/// Lay a FAT 8.3 entry with the archive attribute, first cluster and size,
/// plus create/modify/access stamps, into `out`.
fn layFatEntry(out: *[32]u8, attr: u8, cluster: u32, size: u32) void {
    out.* = @splat(0);
    out[0] = 'R';
    out[11] = attr;
    out[20] = @intCast((cluster >> 16) & 0xFF);
    out[21] = @intCast((cluster >> 24) & 0xFF);
    out[26] = @intCast(cluster & 0xFF);
    out[27] = @intCast((cluster >> 8) & 0xFF);
    std.mem.writeInt(u32, out[28..32], size, .little);
    // creation: date + time + 137 tenths
    std.mem.writeInt(u16, out[13..15], 137, .little);
    std.mem.writeInt(u16, out[14..16], test_time, .little);
    std.mem.writeInt(u16, out[16..18], test_date, .little);
    // access: date only
    std.mem.writeInt(u16, out[18..20], test_date, .little);
    // modify: date + time
    std.mem.writeInt(u16, out[22..24], test_time, .little);
    std.mem.writeInt(u16, out[24..26], test_date, .little);
}

test "entry_to_stat decodes a file entry and zeroes a directory's size" {
    var entry: [32]u8 = undefined;
    var st: stat.Stat = undefined;
    layFatEntry(&entry, 0x20, 0x0001_2345, 0x123);
    stat.entry_to_stat(&entry, &st);
    try std.testing.expectEqual(@as(u8, 0x20), st.attr);
    try std.testing.expect(!st.is_directory);
    try std.testing.expectEqual(@as(u32, 0x0001_2345), st.first_cluster);
    try std.testing.expectEqual(@as(u64, 0x123), st.size_bytes);
    try std.testing.expect(st.created.valid);
    try std.testing.expectEqual(@as(u8, 31), st.created.value.second);
    try std.testing.expectEqual(@as(u8, 37), st.created.value.centisecond);
    try std.testing.expect(st.modified.valid);
    try std.testing.expectEqual(@as(u8, 15), st.modified.value.minute);
    try std.testing.expect(st.accessed.valid);
    try std.testing.expectEqual(@as(u8, 0), st.accessed.value.second);
    try std.testing.expect(!st.accessed.utc_offset_valid);

    // the same entry with the directory bit: size is forced to 0
    layFatEntry(&entry, 0x10, 0x42, 0x999);
    stat.entry_to_stat(&entry, &st);
    try std.testing.expect(st.is_directory);
    try std.testing.expectEqual(@as(u64, 0), st.size_bytes);
    try std.testing.expectEqual(@as(u32, 0x42), st.first_cluster);
}

test "stat answers the volume root without touching a sector" {
    fake.reset(fat16);
    var st: stat.Stat = undefined;
    try std.testing.expectEqual(stat.ok, stat.ra8_fs_stat(&fake.mount_store, "/", &st));
    try std.testing.expect(st.is_directory);
    try std.testing.expectEqual(@as(u32, 5), st.first_cluster);
    try std.testing.expectEqual(@as(u64, 0), st.size_bytes);
    try std.testing.expectEqual(@as(u8, @intCast(c.k_ra8_fs_attr_directory)), st.attr);
    // "" and a slash run name the root too.
    try std.testing.expectEqual(stat.ok, stat.ra8_fs_stat(&fake.mount_store, "", &st));
    try std.testing.expectEqual(stat.ok, stat.ra8_fs_stat(&fake.mount_store, "///", &st));
}

test "stat rejects null pointers and an unmounted handle" {
    fake.reset(fat16);
    var st: stat.Stat = undefined;
    try std.testing.expectEqual(stat.err_null_ptr, stat.ra8_fs_stat(null, "/", &st));
    try std.testing.expectEqual(stat.err_null_ptr, stat.ra8_fs_stat(&fake.mount_store, null, &st));
    try std.testing.expectEqual(stat.err_null_ptr, stat.ra8_fs_stat(&fake.mount_store, "/", null));
    fake.mount_store.in_use = 0;
    try std.testing.expectEqual(stat.err_invalid_state, stat.ra8_fs_stat(&fake.mount_store, "/", &st));
}

test "stat resolves a leaf through the 8.3 find" {
    fake.reset(fat16);
    // the real 8.3 find matches "report.txt" as its 11-byte name field
    var e: [32]u8 = @splat(0);
    layFatEntry(&e, 0x20, 0x77, 0x4242);
    @memcpy(e[0..11], "REPORT\x20\x20TXT");
    @memcpy(fake.sector[0..32], &e);
    var st: stat.Stat = undefined;
    try std.testing.expectEqual(stat.ok, stat.ra8_fs_stat(&fake.mount_store, "report.txt", &st));
    try std.testing.expectEqual(@as(u8, 0x20), st.attr);
    try std.testing.expectEqual(@as(u32, 0x77), st.first_cluster);
    try std.testing.expectEqual(@as(u64, 0x4242), st.size_bytes);
    try std.testing.expect(st.modified.valid);
    try std.testing.expectEqual(@as(u16, 2026), st.modified.value.year);
    // a failing parent lookup surfaces through the resolver
    fake.sector_read_err = c.k_ra8_err_invalid_arg;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_invalid_arg), stat.ra8_fs_stat(&fake.mount_store, "/x/y", &st));
}

test "stat falls back to the long-name scan when the 8.3 find misses" {
    fake.reset(fat16);
    fake.layLongName("alpha.txt", "ALPHA~1.TXT", 0);
    // stamp the laid 8.3 entry (one LFN group -> it starts at byte 32)
    const at: usize = 32;
    std.mem.writeInt(u32, fake.sector[at + 28 ..][0..4], 0xABCD, .little);
    std.mem.writeInt(u16, fake.sector[at + 24 ..][0..2], test_date, .little);
    std.mem.writeInt(u16, fake.sector[at + 22 ..][0..2], test_time, .little);
    // the 8.3 probe misses the ~1 alias, so stat falls to the VFAT chain
    var st: stat.Stat = undefined;
    try std.testing.expectEqual(stat.ok, stat.ra8_fs_stat(&fake.mount_store, "alpha.txt", &st));
    try std.testing.expectEqual(@as(u64, 0xABCD), st.size_bytes);
    try std.testing.expect(st.modified.valid);
    try std.testing.expectEqual(@as(u16, 2026), st.modified.value.year);
    try std.testing.expectEqual(@as(u8, 0x20), st.attr);
    // a name that exists as neither 8.3 nor a laid chain is not found
    fake.reset(fat16);
    // a base over 8 never packs to 8.3, so stat goes to the long-name scan;
    // the name exists as neither 8.3 nor a laid chain, so it is not found
    try std.testing.expectEqual(stat.err_not_found, stat.ra8_fs_stat(&fake.mount_store, "toolongname.txt", &st));
}

test "stat on exFAT reads the File and Stream entries" {
    fake.reset(exfat);
    stageSet("book.epub", 0x20, 0x99, 0x123456);
    std.mem.writeInt(u32, fake.dir[2][8..12], test_packed, .little);
    std.mem.writeInt(u32, fake.dir[2][12..16], test_packed, .little);
    std.mem.writeInt(u32, fake.dir[2][16..20], test_packed, .little);
    fake.dir[2][20] = 137; // create 10-ms increment
    fake.dir[2][22] = 0x80 | 8; // create UTC +2h
    fake.dir[2][23] = 0x80 | 0x7C; // modify UTC -1h
    _ = c.priv_exfat_set_checksum(@ptrCast(&fake.dir[2]), 96);
    var st: stat.Stat = undefined;
    try std.testing.expectEqual(stat.ok, stat.ra8_fs_stat(&fake.mount_store, "book.epub", &st));
    try std.testing.expectEqual(@as(u8, 0x20), st.attr);
    try std.testing.expect(!st.is_directory);
    try std.testing.expectEqual(@as(u32, 0x99), st.first_cluster);
    try std.testing.expectEqual(@as(u64, 0x123456), st.size_bytes);
    try std.testing.expect(st.created.valid);
    try std.testing.expectEqual(@as(u8, 31), st.created.value.second);
    try std.testing.expect(st.created.utc_offset_valid);
    try std.testing.expectEqual(@as(i16, 120), st.created.value.utc_offset_min);
    try std.testing.expect(st.modified.utc_offset_valid);
    try std.testing.expectEqual(@as(i16, -60), st.modified.value.utc_offset_min);
    // access carries no tenth, so its zone byte is still decoded
    try std.testing.expect(st.accessed.valid);
    try std.testing.expect(!st.accessed.utc_offset_valid);
    // the fake's resolver errors surface through
    fake.parent_err = c.k_ra8_err_not_found;
    try std.testing.expectEqual(stat.err_not_found, stat.ra8_fs_stat(&fake.mount_store, "x", &st));
    fake.parent_err = 0;
    fake.find_set_err = c.k_ra8_err_invalid_arg;
    try std.testing.expectEqual(@as(u16, fs.c.k_ra8_err_not_found), stat.ra8_fs_stat(&fake.mount_store, "x", &st));
}

test "path_is_root covers the slash runs" {
    try std.testing.expect(stat.path_is_root(""));
    try std.testing.expect(stat.path_is_root("/"));
    try std.testing.expect(stat.path_is_root("///"));
    try std.testing.expect(!stat.path_is_root("/a"));
    try std.testing.expect(!stat.path_is_root("a/"));
}
