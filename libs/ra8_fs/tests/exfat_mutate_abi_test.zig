//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT mutation helpers, first half (RA8FW-1124): bitmap clearing with
//! the scan hint, set consumption and lookup through a scripted directory
//! cursor, entry rewrite, cluster freeing (contiguous run and FAT chain),
//! the entry-set drop and the unlink guards -- against a scripted backend.

const std = @import("std");
const fs = @import("ra8_fs");
const mu = fs.exfat_mutate;
const c = fs.c;
const al = fs.alloc;

const bps: u32 = 512;
const ok: u16 = c.k_ra8_ok;
const not_found: u16 = c.k_ra8_err_not_found;
const no_mem: u16 = c.k_ra8_err_no_mem;
const invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const access_denied: u16 = c.k_ra8_err_access_denied;
const protocol_error: u16 = c.k_ra8_err_protocol_error;
const first_data: u32 = @intCast(c.k_cluster_first_data);
const entry_bytes: usize = @intCast(c.k_exfat_entry_bytes);
const set_max_entries: usize = @intCast(c.k_exfat_set_max_entries);

const bmp_lba: u64 = 1002;
const dir_cluster: u32 = 7;
const eoc_test: u32 = 0xFFFF_FFFF;

// ---------------------------------------------------------------- fakes

var sec_buf: *[c.k_ra8_fs_sector_max]u8 = fs.fat_bytes.io_sector;
var bmp_store: [4][bps]u8 = @splat(@splat(0));
var bmp_err: u16 = 0;
var bmp_calls: u32 = 0;
var last_write_lba: u64 = 0;
var write_count: u32 = 0;
var write_err: u16 = 0;
var bmp_lba_calls: u32 = 0;

var script: [12][entry_bytes]u8 = undefined;
var script_len: usize = 0;
var script_idx: usize = 0;
var script_err: u16 = 0;

var fat_tbl: [32]u32 = @splat(0);

fn reset() void {
    @memset(sec_buf, 0);
    for (&bmp_store) |*s| s.* = @splat(0);
    bmp_err = 0;
    bmp_calls = 0;
    last_write_lba = 0;
    write_count = 0;
    write_err = 0;
    bmp_lba_calls = 0;
    script_len = 0;
    script_idx = 0;
    script_err = 0;
    script_start = 0;
    root_tree = false;
    last_writes = @splat(@splat(0));
    fat_tbl = @splat(0);
    al.priv_fat_cache_reset();
}

const first_fat_lba: u64 = 900;
const fat_secs: u64 = 2;
var script_start: usize = 0; // slot the first script entry sits in
var root_tree: bool = false; // LBA 0 serves a real root: the dir set + $Bitmap
var last_writes: [8][bps]u8 = undefined; // every sector the backend took

fn layDirSet(buf: *[bps]u8, slot: usize, name: []const u16, clus: u32) void {
    const base = slot * entry_bytes;
    buf[base] = 0x85; // File entry, directory, in use
    buf[base + 1] = 2; // SecondaryCount
    wr32(buf[base..], @intCast(c.k_exfat_off_file_attr), @intCast(c.k_exfat_attr_directory));
    buf[base + entry_bytes] = @intCast(c.k_exfat_entry_stream);
    buf[base + entry_bytes + @as(usize, @intCast(c.k_exfat_strm_off_flags))] = @intCast(c.k_exfat_secflag_no_fat);
    buf[base + entry_bytes + @as(usize, @intCast(c.k_exfat_strm_off_nlen))] = @intCast(name.len);
    wr32(buf[base + entry_bytes ..], @intCast(c.k_exfat_strm_off_clus), clus);
    buf[base + 2 * entry_bytes] = @intCast(c.k_exfat_entry_name);
    for (name, 0..) |u, i| wr16(buf[base + 2 * entry_bytes ..], 2 + i * 2, u);
    for (name.len..15) |i| wr16(buf[base + 2 * entry_bytes ..], 2 + i * 2, 0xFFFF);
}

fn fillScript(lba: u64, buf: *[bps]u8) void {
    @memset(buf, 0);
    if (script_len == 0 and !root_tree) {
        // Root fixture for the free-cluster path: the $Bitmap entry
        // find_bitmap looks for. Cluster N maps to LBA N here, so first
        // cluster bmp_lba lands the bitmap at bmp_lba.
        buf[0] = 0x81; // allocation bitmap
        wr32(buf, @intCast(c.k_exfat_strm_off_clus), @intCast(bmp_lba));
        wr32(buf, @intCast(c.k_exfat_strm_off_dlen), 1024);
        return;
    }
    if (root_tree and lba == 0) {
        // A walkable root served ONLY at LBA 0 (the root cluster maps to
        // LBA 0): the "dir" set points at dir_cluster, the "d" set at
        // cluster 8, and the $Bitmap entry lets unlink's cluster freeing
        // find the bitmap. Cluster N maps to LBA N here.
        const dn_dir = [_]u16{ 'd', 'i', 'r' };
        const dn_d = [_]u16{'d'};
        layDirSet(buf, 0, &dn_dir, dir_cluster);
        layDirSet(buf, 3, &dn_d, 8);
        buf[6 * entry_bytes] = 0x81; // $Bitmap
        wr32(buf[6 * entry_bytes ..], @intCast(c.k_exfat_strm_off_clus), @intCast(bmp_lba));
        wr32(buf[6 * entry_bytes ..], @intCast(c.k_exfat_strm_off_dlen), 1024);
        return;
    }
    var i: usize = 0;
    while (i < script_start) : (i += 1) buf[i * entry_bytes] = entry_eod_b;
    for (script[0..script_len], 0..) |e, k| {
        @memcpy(buf[(script_start + k) * entry_bytes ..][0..entry_bytes], &e);
    }
    var j: usize = script_start + script_len;
    while (j * entry_bytes < bps) : (j += 1) buf[j * entry_bytes] = entry_eod_b;
}

fn fatToSector(buf: *[bps]u8) void {
    @memset(buf, 0);
    for (fat_tbl, 0..) |v, i| wr32(buf, (i + first_data) * 4, v);
}

fn testReadBackend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (lba >= bmp_lba and lba < bmp_lba + 4) {
        if (bmp_err != 0) return bmp_err;
        @memcpy(buf[0..bps], &bmp_store[@intCast(lba - bmp_lba)]);
        bmp_lba_calls += 1;
        return 0;
    }
    if (lba >= first_fat_lba and lba < first_fat_lba + fat_secs) {
        fatToSector(buf[0..bps]);
        return 0;
    }
    if (script_err != 0) return script_err;
    fillScript(lba, buf[0..bps]);
    return 0;
}

fn testWriteBackend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (write_err != 0) return write_err;
    if (write_count < last_writes.len) {
        @memcpy(&last_writes[write_count], buf[0..bps]);
    }
    write_count += 1;
    last_write_lba = lba;
    if (lba >= bmp_lba and lba < bmp_lba + 4) {
        @memcpy(&bmp_store[@intCast(lba - bmp_lba)], buf[0..bps]);
    }
    return 0;
}

fn mkMount() c.ra8_fs_mount_t {
    var m: c.ra8_fs_mount_t = std.mem.zeroes(c.ra8_fs_mount_t);
    m.count_of_clusters = 16;
    // priv_bps reads this: it was the only unset field the volume math
    // divides by, and zero here caused 32 division-by-zero crashes.
    m.bytes_per_sector = bps;
    m.sectors_per_cluster = 1;
    // Cluster N maps to LBA N, so dir clusters and the recorded write
    // positions stay readable in the assertions.
    m.first_data_lba = 2;
    m.first_fat_lba = first_fat_lba;
    m.fat_size_sectors = fat_secs;
    m.num_fats = 1;
    // The real IO path fail-closes on a null backend (not_supported), so
    // every mount carries the scripted one the stub-era seams replaced.
    m.backend.read_block = testReadBackend;
    m.backend.write_block = testWriteBackend;
    return m;
}

fn wr16(p: []u8, at: usize, v: u16) void {
    p[at] = @truncate(v);
    p[at + 1] = @truncate(v >> 8);
}

fn wr32(p: []u8, at: usize, v: u32) void {
    p[at] = @truncate(v);
    p[at + 1] = @truncate(v >> 8);
    p[at + 2] = @truncate(v >> 16);
    p[at + 3] = @truncate(v >> 24);
}

fn wr64(p: []u8, at: usize, v: u64) void {
    wr32(p, at, @truncate(v));
    wr32(p, at + 4, @truncate(v >> 32));
}

fn mkFile(secnt: u8, attr: u8) [entry_bytes]u8 {
    var e: [entry_bytes]u8 = @splat(0);
    e[0] = @intCast(c.k_exfat_entry_file);
    e[1] = secnt;
    e[4] = attr;
    return e;
}

fn mkStream(nlen: u8, first: u32, size: u64, nofat: bool) [entry_bytes]u8 {
    var e: [entry_bytes]u8 = @splat(0);
    e[0] = @intCast(c.k_exfat_entry_stream);
    e[@intCast(c.k_exfat_strm_off_flags)] = if (nofat) @intCast(c.k_exfat_secflag_no_fat) else 0;
    e[@intCast(c.k_exfat_strm_off_nlen)] = nlen;
    wr32(&e, @intCast(c.k_exfat_strm_off_clus), first);
    wr64(&e, @intCast(c.k_exfat_strm_off_dlen), size);
    return e;
}

fn mkName(units: []const u16) [entry_bytes]u8 {
    var e: [entry_bytes]u8 = @splat(0);
    e[0] = @intCast(c.k_exfat_entry_name);
    for (units, 0..) |u, i| {
        wr16(&e, 2 + i * 2, u);
    }
    for (units.len..15) |i| {
        wr16(&e, 2 + i * 2, 0xFFFF);
    }
    return e;
}

fn mkUnits(name: []const u8) [16]u16 {
    var u: [16]u16 = @splat(0);
    for (name, 0..) |ch, i| u[i] = ch;
    return u;
}

// ---------------------------------------------------------------- tests

fn dirDummy() c.exfat_dir_t {
    var d: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    d.cluster = dir_cluster;
    return d;
}

test "bitmap clear clears bits across bytes and lowers the hint per cluster" {
    reset();
    @memset(&bmp_store[0], 0xFF);
    var m = mkMount();
    al.priv_alloc_state_bind(&m);
    const e = mu.priv_exfat_bitmap_clear(&m, bmp_lba, first_data, 9);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 1), write_count);
    try std.testing.expectEqual(bmp_lba, last_write_lba);
    try std.testing.expectEqual(@as(u8, 0x00), bmp_store[0][0]);
    try std.testing.expectEqual(@as(u8, 0xFE), bmp_store[0][1]);
    // The real hint only lowers below its seed, so nine lowerings of the
    // nine cleared clusters leave it pinned at first_data.
    try std.testing.expectEqual(first_data, al.priv_alloc_hint_get(&m));
}

test "bitmap clear propagates the backend error" {
    reset();
    @memset(&bmp_store[0], 0xFF);
    var m = mkMount();
    bmp_err = c.k_ra8_err_not_supported; // k_ra8_err_io gone post-port
    const e = mu.priv_exfat_bitmap_clear(&m, bmp_lba, first_data, 1);
    try std.testing.expectEqual(c.k_ra8_err_not_supported, e);
    try std.testing.expectEqual(@as(u32, 0), write_count);
}

test "take set consumes secondaries, records positions and matches" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkStream(2, 11, 512, false);
    script[1] = mkName(units[0..2]);
    script_len = 2;
    script_start = 2;
    var cur: c.exfat_cursor_t = .{ .cluster = dir_cluster, .entry_in_cluster = 2, .scanned = 0, .contig_end = 0 };
    var pos: [set_max_entries]c.exfat_setpos_t = std.mem.zeroes([set_max_entries]c.exfat_setpos_t);
    var strm_copy: [entry_bytes]u8 = @splat(0);
    var matched: u8 = 0;
    const e = mu.internal_exfat_take_set(&m0, &cur, &units, 2, 2, &pos, &strm_copy, &matched);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u8, 1), matched);
    try std.testing.expectEqual(@as(u8, @intCast(c.k_exfat_entry_stream)), strm_copy[0]);
    try std.testing.expectEqual(dir_cluster, pos[1].cluster);
    try std.testing.expectEqual(@as(u32, 2), pos[1].index);
    try std.testing.expectEqual(dir_cluster, pos[2].cluster);
    try std.testing.expectEqual(@as(u32, 3), pos[2].index);
}

test "take set reports a name-length mismatch without stopping" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkStream(3, 11, 512, false);
    script[1] = mkName(units[0..2]);
    script_len = 2;
    script_start = 2;
    var cur: c.exfat_cursor_t = .{ .cluster = dir_cluster, .entry_in_cluster = 2, .scanned = 0, .contig_end = 0 };
    var pos: [set_max_entries]c.exfat_setpos_t = std.mem.zeroes([set_max_entries]c.exfat_setpos_t);
    var strm_copy: [entry_bytes]u8 = @splat(0);
    var matched: u8 = 1;
    const e = mu.internal_exfat_take_set(&m0, &cur, &units, 2, 2, &pos, &strm_copy, &matched);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u8, 0), matched);
    // the cursor still consumed both secondaries
    try std.testing.expectEqual(@as(u32, 4), cur.entry_in_cluster);
}

test "try set rejects oversized sets and reports not-found on mismatch" {
    reset();
    const units = mkUnits("AB");
    var pos: [set_max_entries]c.exfat_setpos_t = std.mem.zeroes([set_max_entries]c.exfat_setpos_t);
    var count: u32 = 0;
    var file_copy: [entry_bytes]u8 = @splat(0);
    var strm_copy: [entry_bytes]u8 = @splat(0);

    const file_big = mkFile(3, 0);
    script[0] = mkStream(2, 11, 512, false);
    script[1] = mkName(units[0..2]);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    var cur: c.exfat_cursor_t = .{ .cluster = dir_cluster, .entry_in_cluster = 0, .scanned = 0, .contig_end = 0 };
    var e = mu.internal_exfat_try_set(&m0, &cur, pos[0], &file_big, &units, 2, &pos, 2, &count, &file_copy, &strm_copy);
    try std.testing.expectEqual(no_mem, e);

    reset();
    // A 3-secondary File entry consumes ALL three secondaries: stream plus
    // two Name entries. The stub-era fixture stopped at one Name, so the
    // real walk hit the EOD and reported not-found.
    script[0] = mkStream(2, 11, 512, false);
    script[1] = mkName(units[0..2]);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    cur = .{ .cluster = dir_cluster, .entry_in_cluster = 0, .scanned = 0, .contig_end = 0 };
    e = mu.internal_exfat_try_set(&m0, &cur, pos[0], &file_big, &units, 2, &pos, 4, &count, &file_copy, &strm_copy);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 4), count); // 1 + SecondaryCount(3)
    try std.testing.expectEqual(@as(u8, @intCast(c.k_exfat_entry_file)), file_copy[0]);

    reset();
    script[0] = mkStream(5, 11, 512, false);
    script[1] = mkName(units[0..2]);
    script_len = 2;
    cur = .{ .cluster = dir_cluster, .entry_in_cluster = 0, .scanned = 0, .contig_end = 0 };
    e = mu.internal_exfat_try_set(&m0, &cur, pos[0], &file_big, &units, 2, &pos, 4, &count, &file_copy, &strm_copy);
    try std.testing.expectEqual(not_found, e);
}

test "find set skips non-file entries and locates a matching set" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkName(units[0..2]); // a stray Name entry is skipped
    script[1] = mkFile(2, 0);
    script[2] = mkStream(2, 11, 512, false);
    script[3] = mkName(units[0..2]);
    script_len = 4;
    var m = mkMount();
    const dir = dirDummy();
    var pos: [set_max_entries]c.exfat_setpos_t = std.mem.zeroes([set_max_entries]c.exfat_setpos_t);
    var count: u32 = 0;
    var file_copy: [entry_bytes]u8 = @splat(0);
    var strm_copy: [entry_bytes]u8 = @splat(0);
    const e = mu.priv_exfat_find_set(&m, &dir, "AB", &pos, set_max_entries, &count, &file_copy, &strm_copy);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 3), count);
    try std.testing.expectEqual(dir_cluster, pos[0].cluster);
    try std.testing.expectEqual(@as(u32, 1), pos[0].index);
    try std.testing.expectEqual(@as(u32, 2), pos[1].index);
    try std.testing.expectEqual(@as(u32, 3), pos[2].index);
    try std.testing.expectEqual(@as(u8, @intCast(c.k_exfat_entry_file)), file_copy[0]);
    try std.testing.expectEqual(@as(u8, @intCast(c.k_exfat_entry_stream)), strm_copy[0]);
}

test "find set handles a leading slash and stops at end-of-directory" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkFile(2, 0);
    script[1] = mkStream(2, 11, 512, false);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    var m = mkMount();
    const dir = dirDummy();
    var pos: [set_max_entries]c.exfat_setpos_t = std.mem.zeroes([set_max_entries]c.exfat_setpos_t);
    var count: u32 = 0;
    var file_copy: [entry_bytes]u8 = @splat(0);
    var strm_copy: [entry_bytes]u8 = @splat(0);
    const e = mu.priv_exfat_find_set(&m, &dir, "/AB", &pos, set_max_entries, &count, &file_copy, &strm_copy);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 3), count);

    reset();
    script[0] = mkName(units[0..2]);
    script_len = 1;
    const e2 = mu.priv_exfat_find_set(&m, &dir, "AB", &pos, set_max_entries, &count, &file_copy, &strm_copy);
    try std.testing.expectEqual(not_found, e2);
}

test "put entry rewrites the recorded position" {
    reset();
    var m = mkMount();
    var where: c.exfat_setpos_t = .{ .cluster = 9, .index = 4 };
    var entry: [entry_bytes]u8 = @splat(0);
    entry[0] = 0x05;
    const e = mu.internal_exfat_put_entry(&m, &where, &entry);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 1), write_count);
    try std.testing.expectEqual(@as(u64, 9), last_write_lba); // cluster 9 -> LBA 9
    try std.testing.expectEqual(@as(u8, 0x05), last_writes[0][4 * entry_bytes]);
}

test "free clusters clears a contiguous run sized from the 64-bit length" {
    reset();
    var m = mkMount();
    al.priv_alloc_state_bind(&m);
    @memset(&bmp_store[0], 0xFF); // the on-disk bitmap: clusters 0..15 used
    const strm = mkStream(2, 5, 1024, true);
    const e = mu.priv_exfat_free_clusters(&m, &strm);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 1), write_count);
    // clusters 5 and 6 -> indices 3 and 4 -> byte 0 bits 3 and 4
    try std.testing.expectEqual(@as(u8, 0xE7), bmp_store[0][0]);
    try std.testing.expectEqual(first_data, al.priv_alloc_hint_get(&m));
}

test "free clusters walks a FAT chain to end-of-chain" {
    reset();
    var m = mkMount();
    @memset(&bmp_store[0], 0xFF); // the on-disk bitmap: clusters 0..15 used
    const strm = mkStream(2, 5, 1024, false);
    fat_tbl[5 - first_data] = 6;
    fat_tbl[6 - first_data] = eoc_test;
    const e = mu.priv_exfat_free_clusters(&m, &strm);
    try std.testing.expectEqual(ok, e);
    // one write per bitmap_clear call: the chain frees clusters 5 and 6 in
    // two clears, so two sectors hit the backend.
    try std.testing.expectEqual(@as(u32, 2), write_count);
    try std.testing.expectEqual(@as(u8, 0xE7), bmp_store[0][0]);
}

test "free clusters reports a chain that never ends" {
    reset();
    var m = mkMount();
    const strm = mkStream(2, 5, 1024, false);
    fat_tbl[5 - first_data] = 6;
    fat_tbl[6 - first_data] = 7;
    const e = mu.priv_exfat_free_clusters(&m, &strm);
    try std.testing.expectEqual(protocol_error, e);
}

test "free clusters ignores a tiny first cluster and a zero length" {
    reset();
    var m = mkMount();
    const tiny = mkStream(2, 1, 512, true);
    try std.testing.expectEqual(ok, mu.priv_exfat_free_clusters(&m, &tiny));
    const empty = mkStream(2, 5, 0, true);
    try std.testing.expectEqual(ok, mu.priv_exfat_free_clusters(&m, &empty));
    try std.testing.expectEqual(@as(u32, 0), bmp_lba_calls);
    try std.testing.expectEqual(@as(u32, 0), write_count);
}

test "drop set clears the in-use bit at each recorded position" {
    reset();
    script_start = 4; // the entries must sit where the recorded positions point
    var m = mkMount();
    var pos: [2]c.exfat_setpos_t = .{ .{ .cluster = 9, .index = 4 }, .{ .cluster = 9, .index = 5 } };
    script[0] = mkFile(2, 0);
    script[1] = mkStream(2, 11, 512, false);
    script_len = 2;
    const e = mu.priv_exfat_drop_set(&m, &pos, 2);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 2), write_count);
    try std.testing.expectEqual(@as(u64, 9), last_write_lba);
    try std.testing.expectEqual(@as(u8, 0x05), last_writes[0][4 * entry_bytes]);
    try std.testing.expectEqual(entry_stream_b & ~inuse, last_writes[1][5 * entry_bytes]);
}

test "unlink at refuses directories and read-only files" {
    reset();
    var m = mkMount();
    const dir = dirDummy();
    script[0] = mkFile(2, @intCast(c.k_exfat_attr_directory));
    script[1] = mkStream(2, 11, 512, false);
    script[2] = mkName(mkUnits("AB")[0..2]);
    script_len = 3;
    const e = mu.priv_exfat_unlink_at(&m, &dir, "AB");
    try std.testing.expectEqual(invalid_arg, e);
    try std.testing.expectEqual(@as(u32, 0), write_count);

    reset();
    script[0] = mkFile(2, @intCast(c.k_exfat_attr_read_only));
    script[1] = mkStream(2, 11, 512, false);
    script[2] = mkName(mkUnits("AB")[0..2]);
    script_len = 3;
    const e2 = mu.priv_exfat_unlink_at(&m, &dir, "AB");
    try std.testing.expectEqual(access_denied, e2);
}

test "unlink at drops the set and frees the clusters" {
    reset();
    root_tree = true; // the root carries the $Bitmap the freeing path finds
    var m = mkMount();
    @memset(&bmp_store[0], 0xFF); // the on-disk bitmap: clusters 0..15 used
    const dir = dirDummy();
    const units = mkUnits("AB");
    script[0] = mkFile(2, 0x20);
    script[1] = mkStream(2, 5, 1024, true);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    const e = mu.priv_exfat_unlink_at(&m, &dir, "AB");
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 4), write_count); // 3 set drops + the bitmap
    try std.testing.expectEqual(@as(u8, 0xE7), bmp_store[0][0]);
}

test "unlink refuses an empty leaf and unlinks a file by path" {
    reset();
    var m = mkMount();
    const e = mu.priv_exfat_unlink(&m, "/");
    try std.testing.expectEqual(invalid_arg, e);

    reset();
    root_tree = true;
    const units = mkUnits("AB");
    script[0] = mkFile(2, 0x20);
    script[1] = mkStream(2, 5, 1024, true);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    const e2 = mu.priv_exfat_unlink(&m, "/dir/AB");
    try std.testing.expectEqual(ok, e2);
    try std.testing.expectEqual(@as(u32, 4), write_count); // 3 set drops + the bitmap
}

// m0 seeds through mkMount: a zeroed mount had bytes_per_sector 0, and the
// rename/list paths that walk directories with it divided by it in priv_fat_get.
var m0: c.ra8_fs_mount_t = mkMount();

// ------------------------------------------------- rename / listdir state

const exists: u16 = c.k_ra8_err_exists;
const not_supported: u16 = c.k_ra8_err_not_supported;
const invalid_size: u16 = c.k_ra8_err_invalid_size;

const entry_file_b: u8 = @intCast(c.k_exfat_entry_file);
const entry_stream_b: u8 = @intCast(c.k_exfat_entry_stream);
const entry_name_b: u8 = @intCast(c.k_exfat_entry_name);
const entry_eod_b: u8 = @intCast(c.k_exfat_entry_eod);
const inuse: u8 = @intCast(c.k_exfat_inuse_bit);
const name_u8_cap: usize = @intCast(c.k_exfat_name_u8_cap);
const scan_limit: u32 = @intCast(c.k_exfat_scan_limit);
const off_file_attr_i: usize = @intCast(c.k_exfat_off_file_attr);
const strm_nlen_i: usize = @intCast(c.k_exfat_strm_off_nlen);
const strm_dlen_i: usize = @intCast(c.k_exfat_strm_off_dlen);
const name_off_i: usize = @intCast(c.k_exfat_name_off);
const file_csum_i: usize = @intCast(c.k_exfat_off_file_csum);
const strm_hash_i: usize = @intCast(c.k_exfat_off_strm_hash);

var find_err: u16 = not_found;
var space_err: u16 = 0;
var space_clus: u32 = 0;
var space_idx: u32 = 0;
var hash_calls: u32 = 0;
var stamp_calls: u32 = 0;
var csum_calls: u32 = 0;
var utf_err: u16 = 0;
var cb_n: usize = 0;
var cb_names: [8][32]u8 = undefined;

fn rd16v(p: []const u8, at: usize) u16 {
    return @as(u16, p[at]) | (@as(u16, p[at + 1]) << 8);
}

fn rd32v(p: []const u8, at: usize) u32 {
    return @as(u32, p[at]) | (@as(u32, p[at + 1]) << 8) | (@as(u32, p[at + 2]) << 16) | (@as(u32, p[at + 3]) << 24);
}

fn mkGarbage(t: u8) [entry_bytes]u8 {
    var e: [entry_bytes]u8 = @splat(0);
    e[0] = t;
    return e;
}

fn mkUnits19(name: []const u8) [19]u16 {
    var u: [19]u16 = @splat(0);
    for (name, 0..) |ch, i| u[i] = ch;
    return u;
}

fn freshCursor(at: u32) c.exfat_cursor_t {
    return .{ .cluster = dir_cluster, .entry_in_cluster = at, .scanned = 0, .contig_end = 0 };
}

fn listCb(name: [*c]const u8, attr: u8, size: u64, ctx: ?*anyopaque) callconv(.c) void {
    _ = attr;
    _ = size;
    _ = ctx;
    var i: usize = 0;
    while (name[i] != 0) : (i += 1) {}
    @memcpy(cb_names[cb_n][0..i], name[0..i]);
    cb_names[cb_n][i] = 0; // the callback buffer is the test's own: terminate it
    cb_n += 1;
}

// ------------------------------------------------------------ rename tests

test "build rename set keeps every non-name field and pads to entry boundaries" {
    reset();
    var file_e = mkFile(2, 0x20);
    file_e[12] = 0xAB; // LastModified rides across untouched
    file_e[21] = 0x99; // LastModifiedUtc rides across untouched
    var strm_e = mkStream(2, 11, 512, false);
    strm_e[2] = 7; // stale SecondaryCount is overwritten
    const units = mkUnits("XY");
    var set: [c.k_exfat_max_set_bytes]u8 = @splat(0);
    const n = mu.internal_exfat_build_rename_set(&set, &file_e, &strm_e, &units, 2);
    try std.testing.expectEqual(@as(u32, 96), n);
    try std.testing.expectEqual(@as(u8, 2), set[1]); // SecondaryCount = 1 + 1 name entry
    try std.testing.expectEqual(file_e[12], set[12]);
    try std.testing.expectEqual(file_e[21], set[21]);
    // LastAccessed is stamped for real: no clock bound in tests, so the
    // epoch reading (1980-01-01 00:00:00, 0x210000) is what lands, with
    // OffsetValid clear.
    try std.testing.expectEqual(@as(u32, 0x210000), rd32v(&set, @intCast(c.k_exfat_off_file_atime)));
    try std.testing.expectEqual(@as(u8, 0), set[@intCast(c.k_exfat_off_file_autc)]);
    try std.testing.expectEqual(@as(u16, 0xD11F), rd16v(&set, file_csum_i)); // real set checksum over the whole set (epoch access stamp included)
    try std.testing.expectEqual(@as(u8, 2), set[entry_bytes + strm_nlen_i]);
    try std.testing.expectEqual(@as(u16, 0x8037), rd16v(&set, entry_bytes + strm_hash_i)); // real rotate-add hash of "XY"
    try std.testing.expectEqual(entry_name_b, set[2 * entry_bytes]);
    try std.testing.expectEqual(@as(u8, 'X'), set[2 * entry_bytes + name_off_i]);
    try std.testing.expectEqual(@as(u8, 0), set[2 * entry_bytes + name_off_i + 1]);
    try std.testing.expectEqual(@as(u8, 'Y'), set[2 * entry_bytes + name_off_i + 2]);
    try std.testing.expectEqual(@as(u8, 0), set[2 * entry_bytes + name_off_i + 4]);
    // The real port writes the stamp and checksum it computes, not the
    // stub-era counter knobs, so the knobs are retired here.
}

test "build rename set grows a second name entry with zero padding" {
    reset();
    var file_e = mkFile(2, 0x20);
    var strm_e = mkStream(2, 11, 512, false);
    const units = mkUnits19("ABCDEFGHIJKLMNOPQRS"); // 19 units
    var set: [c.k_exfat_max_set_bytes]u8 = @splat(0);
    const n = mu.internal_exfat_build_rename_set(&set, &file_e, &strm_e, &units, 19);
    try std.testing.expectEqual(@as(u32, 128), n);
    try std.testing.expectEqual(@as(u8, 3), set[1]); // SecondaryCount = 1 + 2
    try std.testing.expectEqual(@as(u8, 19), set[entry_bytes + strm_nlen_i]);
    try std.testing.expectEqual(@as(u8, 'A'), set[2 * entry_bytes + name_off_i]);
    try std.testing.expectEqual(@as(u8, 'O'), set[2 * entry_bytes + name_off_i + 14 * 2]);
    try std.testing.expectEqual(entry_name_b, set[3 * entry_bytes]);
    try std.testing.expectEqual(@as(u8, 'P'), set[3 * entry_bytes + name_off_i]);
    try std.testing.expectEqual(@as(u8, 'S'), set[3 * entry_bytes + name_off_i + 3 * 2]);
    try std.testing.expectEqual(@as(u8, 0), set[3 * entry_bytes + name_off_i + 4 * 2]);
}

test "rename prepare resolves a shared parent and both leaves" {
    reset();
    root_tree = true;
    var parent: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var ol: [*c]const u8 = null;
    var nl: [*c]const u8 = null;
    const e = mu.internal_exfat_rename_prepare(&m0, "/d/AB", "/d/XY", &parent, &ol, &nl);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqualStrings("AB", std.mem.sliceTo(ol, 0));
    try std.testing.expectEqualStrings("XY", std.mem.sliceTo(nl, 0));
}

test "rename prepare refuses different parents and empty leaves" {
    reset();
    root_tree = true;
    var parent: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var ol: [*c]const u8 = null;
    var nl: [*c]const u8 = null;
    const e = mu.internal_exfat_rename_prepare(&m0, "/d/AB", "/dir/XY", &parent, &ol, &nl);
    try std.testing.expectEqual(not_supported, e);

    const e2 = mu.internal_exfat_rename_prepare(&m0, "/d/AB", "/d/", &parent, &ol, &nl);
    try std.testing.expectEqual(invalid_arg, e2);
}

test "place rename rewrites every slot when the entry count is unchanged" {
    reset();
    var pos: [3]c.exfat_setpos_t = undefined;
    // The old set lives in the dir cluster the new scan runs against, so the
    // retire writes and the fresh set land on the same cluster.
    pos[0] = .{ .cluster = dir_cluster, .index = 0 };
    pos[1] = .{ .cluster = dir_cluster, .index = 1 };
    pos[2] = .{ .cluster = dir_cluster, .index = 2 };
    var set: [96]u8 = @splat(0);
    set[0] = entry_file_b;
    set[32] = entry_stream_b;
    set[64] = entry_name_b;
    const dir = dirDummy();
    const e = mu.internal_exfat_place_rename(&m0, &dir, &pos, 3, &set, 96);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 3), write_count); // dir cluster -> LBA 7
    try std.testing.expectEqual(@as(u64, dir_cluster), last_write_lba);
    const fresh: [3]u8 = .{ entry_file_b, entry_stream_b, entry_name_b };
    var k: usize = 0;
    while (k < 3) : (k += 1) {
        try std.testing.expectEqual(fresh[k], last_writes[k][k * entry_bytes]);
    }
}

test "place rename relocates a longer set, then retires the old one" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkFile(2, 0x20);
    script[1] = mkStream(2, 5, 1024, true);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    var pos: [3]c.exfat_setpos_t = undefined;
    // The old set lives in the dir cluster the new scan runs against, so the
    // retire writes and the fresh set land on the same cluster.
    pos[0] = .{ .cluster = dir_cluster, .index = 0 };
    pos[1] = .{ .cluster = dir_cluster, .index = 1 };
    pos[2] = .{ .cluster = dir_cluster, .index = 2 };
    var set: [128]u8 = @splat(0);
    set[0] = 0x99;
    const dir = dirDummy();
    const e = mu.internal_exfat_place_rename(&m0, &dir, &pos, 3, &set, 128);
    try std.testing.expectEqual(ok, e);
    // The real allocator finds the first free run: the old set sits at
    // slots 0-2 of the dir cluster, so four free slots start at index 3 of
    // that same cluster -- no grow needed. Four set writes plus the three
    // retire writes.
    try std.testing.expectEqual(@as(u32, 7), write_count);
    try std.testing.expectEqual(@as(u64, dir_cluster), last_write_lba); // dir cluster -> LBA 7
    try std.testing.expectEqual(@as(u8, 0x99), last_writes[0][3 * entry_bytes]); // the run opens at index 3
    const dropped: [3]u8 = .{ entry_file_b, entry_stream_b, entry_name_b };
    var k: usize = 0;
    while (k < 3) : (k += 1) {
        try std.testing.expectEqual(dropped[k] & ~inuse, last_writes[k + 4][k * entry_bytes]);
    }
}

test "place rename propagates the free-slot failure" {
    reset();
    // The dir read itself fails, so the free-slot scan dies before it can
    // look: the real place_rename reports that error untouched.
    script_err = c.k_ra8_err_not_supported;
    var pos: [3]c.exfat_setpos_t = undefined;
    // The old set lives in the dir cluster the new scan runs against, so the
    // retire writes and the fresh set land on the same cluster.
    pos[0] = .{ .cluster = dir_cluster, .index = 0 };
    pos[1] = .{ .cluster = dir_cluster, .index = 1 };
    pos[2] = .{ .cluster = dir_cluster, .index = 2 };
    var set: [128]u8 = @splat(0);
    const dir = dirDummy();
    const e = mu.internal_exfat_place_rename(&m0, &dir, &pos, 3, &set, 128);
    try std.testing.expectEqual(c.k_ra8_err_not_supported, e);
    try std.testing.expectEqual(@as(u32, 0), write_count);
}

test "rename refuses an existing new name before anything is written" {
    reset();
    root_tree = true;
    const units = mkUnits("AB");
    // The dir holds both sets: the old "AB" and an existing "XY".
    script[0] = mkFile(2, 0x20);
    script[1] = mkStream(2, 5, 1024, false);
    script[2] = mkName(units[0..2]);
    script[3] = mkFile(2, 0x20);
    script[4] = mkStream(2, 6, 512, false);
    script[5] = mkName(mkUnits("XY")[0..2]);
    script_len = 6;
    const e = mu.priv_exfat_rename(&m0, "/d/AB", "/d/XY");
    try std.testing.expectEqual(exists, e);
    try std.testing.expectEqual(@as(u32, 0), write_count);
}

test "rename refuses different parents" {
    reset();
    root_tree = true;
    const e = mu.priv_exfat_rename(&m0, "/d/AB", "/dir/XY");
    try std.testing.expectEqual(not_supported, e);
    try std.testing.expectEqual(@as(u32, 0), write_count);
}

test "rename honors the read-only attribute" {
    reset();
    root_tree = true;
    const units = mkUnits("AB");
    script[0] = mkFile(2, @intCast(c.k_exfat_attr_read_only));
    script[1] = mkStream(2, 5, 1024, false);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    const e = mu.priv_exfat_rename(&m0, "/d/AB", "/d/XY");
    try std.testing.expectEqual(access_denied, e);
    try std.testing.expectEqual(@as(u32, 0), write_count);
}

test "rename rebuilds the set and places it when the count is unchanged" {
    reset();
    root_tree = true;
    const units = mkUnits("AB");
    script[0] = mkFile(2, 0x20);
    script[1] = mkStream(2, 5, 1024, false);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    const e = mu.priv_exfat_rename(&m0, "/d/AB", "/d/XY");
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 3), write_count);
    // The count-unchanged path rewrites IN PLACE: the set stays in use, so
    // the type bytes are live entries, not retired ones. The real hash,
    // checksum, and access stamp are what the written slots carry.
    try std.testing.expectEqual(entry_file_b, last_writes[0][0 * entry_bytes]);
    try std.testing.expectEqual(entry_stream_b, last_writes[1][1 * entry_bytes]);
    try std.testing.expectEqual(entry_name_b, last_writes[2][2 * entry_bytes]);
    try std.testing.expectEqual(@as(u8, 'X'), last_writes[2][2 * entry_bytes + name_off_i]);
    try std.testing.expectEqual(@as(u8, 'Y'), last_writes[2][2 * entry_bytes + name_off_i + 2]);
    try std.testing.expectEqual(@as(u16, 0x8037), rd16v(&last_writes[1], entry_bytes + strm_hash_i));
    try std.testing.expectEqual(@as(u32, 0x210000), rd32v(&last_writes[0], @intCast(c.k_exfat_off_file_atime)));
    try std.testing.expectEqual(@as(u16, 0x51D2), rd16v(&last_writes[0], file_csum_i));
}

// --------------------------------------------------------- listdir tests

test "gather name consumes secondaries and converts the units once" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkName(units[0..2]);
    script[1] = mkGarbage(0x99); // a non-Name secondary is skipped
    script_len = 2;
    script_start = 2;
    var cur = freshCursor(2);
    var name: [name_u8_cap]u8 = @splat(0);
    const e = mu.internal_exfat_gather_name(&m0, &cur, 3, 2, &name, @intCast(name_u8_cap));
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u8, 'A'), name[0]);
    try std.testing.expectEqual(@as(u8, 'B'), name[1]);
    try std.testing.expectEqual(@as(u8, 0), name[2]);
    try std.testing.expectEqual(@as(u32, 2), cur.scanned); // both secondaries consumed
}

test "gather name yields nothing for units no UTF-8 string encodes" {
    reset();
    // A surrogate half is a unit no UTF-8 string encodes: the gatherer
    // reports ok and emits nothing rather than mojibake.
    var ne = mkName(mkUnits("X")[0..1]);
    wr16(&ne, name_off_i, 0xD800);
    script[0] = ne;
    script_len = 1;
    var cur = freshCursor(0);
    var name: [name_u8_cap]u8 = @splat(0);
    var e = mu.internal_exfat_gather_name(&m0, &cur, 2, 1, &name, @intCast(name_u8_cap));
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u8, 0), name[0]);

    // An encodable non-ASCII unit converts: E9 is "e" with an acute accent
    // (two UTF-8 bytes 0xC3 0xA9), not garbage to refuse.
    ne = mkName(mkUnits("X")[0..1]);
    wr16(&ne, name_off_i, 0x00E9);
    script[0] = ne;
    script_len = 1;
    cur = freshCursor(0);
    name = @splat(0);
    e = mu.internal_exfat_gather_name(&m0, &cur, 2, 1, &name, @intCast(name_u8_cap));
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u8, 0xC3), name[0]);
    try std.testing.expectEqual(@as(u8, 0xA9), name[1]);
    try std.testing.expectEqual(@as(u8, 0), name[2]);
}

test "gather name propagates a backend failure mid-set" {
    reset();
    script_err = c.k_ra8_err_not_supported;
    var cur = freshCursor(0);
    var name: [name_u8_cap]u8 = @splat(0);
    const e = mu.internal_exfat_gather_name(&m0, &cur, 3, 2, &name, @intCast(name_u8_cap));
    try std.testing.expectEqual(c.k_ra8_err_not_supported, e);
}

test "list emit reports a directory with zero size and a file with its bytes" {
    reset();
    const units = mkUnits("HI");
    var file_e = mkFile(2, @intCast(c.k_exfat_attr_directory));
    script[0] = mkStream(2, 5, 4096, true); // a directory's allocation, not a size
    script[1] = mkName(units[0..2]);
    script_len = 2;
    var cur = freshCursor(0);
    var out: c.ra8_fs_dirent_t = std.mem.zeroes(c.ra8_fs_dirent_t);
    var present = false;
    const e = mu.internal_exfat_list_emit(&m0, &cur, &file_e, &out, &present);
    try std.testing.expectEqual(ok, e);
    try std.testing.expect(present);
    try std.testing.expectEqual(@as(u64, 0), out.size_bytes);
    try std.testing.expectEqual(@as(u8, @intCast(c.k_exfat_attr_directory)), out.attr);
    try std.testing.expectEqualStrings("HI", std.mem.sliceTo(&out.name, 0));

    reset();
    file_e = mkFile(2, 0x20);
    script[0] = mkStream(2, 5, 1234, true);
    script[1] = mkName(units[0..2]);
    script_len = 2;
    cur = freshCursor(0);
    out = std.mem.zeroes(c.ra8_fs_dirent_t);
    present = false;
    const e2 = mu.internal_exfat_list_emit(&m0, &cur, &file_e, &out, &present);
    try std.testing.expectEqual(ok, e2);
    try std.testing.expect(present);
    try std.testing.expectEqual(@as(u64, 1234), out.size_bytes);
}

test "list emit skips a non-stream secondary and an unconvertible name" {
    reset();
    var file_e = mkFile(2, 2); // SecondaryCount says one more entry follows
    script[0] = mkGarbage(0x55); // not a Stream entry
    script_len = 1;
    var cur = freshCursor(0);
    var out: c.ra8_fs_dirent_t = std.mem.zeroes(c.ra8_fs_dirent_t);
    var present = false;
    const e = mu.internal_exfat_list_emit(&m0, &cur, &file_e, &out, &present);
    try std.testing.expectEqual(ok, e);
    try std.testing.expect(!present);

    reset();
    var ne = mkName(mkUnits("X")[0..1]);
    wr16(&ne, name_off_i, 0xD800); // a surrogate: no UTF-8 string encodes it
    script[0] = mkStream(2, 5, 100, false);
    script[1] = ne;
    script_len = 2;
    file_e = mkFile(2, 0x20);
    cur = freshCursor(0);
    out = std.mem.zeroes(c.ra8_fs_dirent_t);
    present = false;
    const e2 = mu.internal_exfat_list_emit(&m0, &cur, &file_e, &out, &present);
    try std.testing.expectEqual(ok, e2);
    try std.testing.expect(!present);
}

test "dir next skips junk, emits a set and ends at EOD" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkGarbage(0x55);
    script[1] = mkFile(2, 0x20);
    script[2] = mkStream(2, 5, 1234, true);
    script[3] = mkName(units[0..2]);
    script[4] = mkGarbage(entry_eod_b);
    script_len = 5;
    var cur = freshCursor(0);
    var out: c.ra8_fs_dirent_t = std.mem.zeroes(c.ra8_fs_dirent_t);
    var present = false;
    const e = mu.priv_exfat_dir_next(&m0, &cur, &out, &present);
    try std.testing.expectEqual(ok, e);
    try std.testing.expect(present);
    try std.testing.expectEqualStrings("AB", std.mem.sliceTo(&out.name, 0));
    try std.testing.expectEqual(@as(u64, 1234), out.size_bytes);

    var out2: c.ra8_fs_dirent_t = std.mem.zeroes(c.ra8_fs_dirent_t);
    var present2 = false;
    const e2 = mu.priv_exfat_dir_next(&m0, &cur, &out2, &present2);
    try std.testing.expectEqual(ok, e2);
    try std.testing.expect(!present2);
}

test "dir next reports a clean end when the backend runs dry" {
    reset();
    var cur = freshCursor(0);
    var out: c.ra8_fs_dirent_t = std.mem.zeroes(c.ra8_fs_dirent_t);
    var present = true;
    const e = mu.priv_exfat_dir_next(&m0, &cur, &out, &present);
    try std.testing.expectEqual(ok, e);
    try std.testing.expect(!present);
}

test "dir next reports invalid size past the scan limit" {
    reset();
    var cur = freshCursor(0);
    cur.scanned = scan_limit;
    var out: c.ra8_fs_dirent_t = std.mem.zeroes(c.ra8_fs_dirent_t);
    var present = false;
    const e = mu.priv_exfat_dir_next(&m0, &cur, &out, &present);
    try std.testing.expectEqual(invalid_size, e);
}

test "listdir walks a whole directory through the callback" {
    reset();
    const units_ab = mkUnits("AB");
    const units_cd = mkUnits("CD");
    script[0] = mkFile(2, 0x20);
    script[1] = mkStream(2, 5, 100, false);
    script[2] = mkName(units_ab[0..2]);
    script[3] = mkFile(2, @intCast(c.k_exfat_attr_directory));
    script[4] = mkStream(2, 6, 4096, true);
    script[5] = mkName(units_cd[0..2]);
    script[6] = mkGarbage(entry_eod_b);
    script_len = 7;
    cb_n = 0;
    const dir = dirDummy();
    const e = mu.priv_exfat_listdir(&m0, &dir, listCb, null);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(usize, 2), cb_n);
    try std.testing.expectEqualStrings("AB", std.mem.sliceTo(&cb_names[0], 0));
    try std.testing.expectEqualStrings("CD", std.mem.sliceTo(&cb_names[1], 0));
}
