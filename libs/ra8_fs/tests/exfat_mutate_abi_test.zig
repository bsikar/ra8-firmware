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

var sec_buf: [c.k_ra8_fs_sector_max]u8 = @splat(0);
var bmp_store: [4][bps]u8 = @splat(@splat(0));
var bmp_err: u16 = 0;
var bmp_calls: u32 = 0;
var last_write_lba: u64 = 0;
var write_count: u32 = 0;
var write_err: u16 = 0;
var hint_calls: [16]u32 = @splat(0);
var hint_count: u32 = 0;
var bmp_lba_calls: u32 = 0;

var script: [12][entry_bytes]u8 = undefined;
var script_len: usize = 0;
var script_idx: usize = 0;
var script_err: u16 = 0;

var fat_tbl: [32]u32 = @splat(0);

var wdir_cluster: [8]u32 = @splat(0);
var wdir_index: [8]u32 = @splat(0);
var wdir_entry0: [8]u8 = @splat(0);
var wdir_count: usize = 0;

fn reset() void {
    for (&sec_buf) |*b| b.* = 0;
    for (&bmp_store) |*s| s.* = @splat(@splat(0));
    bmp_err = 0;
    bmp_calls = 0;
    last_write_lba = 0;
    write_count = 0;
    write_err = 0;
    hint_calls = @splat(0);
    hint_count = 0;
    bmp_lba_calls = 0;
    script_len = 0;
    script_idx = 0;
    script_err = 0;
    fat_tbl = @splat(0);
    wdir_cluster = @splat(0);
    wdir_index = @splat(0);
    wdir_entry0 = @splat(0);
    wdir_count = 0;
}

fn mkMount() c.ra8_fs_mount_t {
    var m: c.ra8_fs_mount_t = std.mem.zeroes(c.ra8_fs_mount_t);
    m.count_of_clusters = 16;
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
    var e: [entry_bytes]u8 = @splat(@splat(0));
    e[0] = @intCast(c.k_exfat_entry_file);
    e[1] = secnt;
    e[4] = attr;
    return e;
}

fn mkStream(nlen: u8, first: u32, size: u64, nofat: bool) [entry_bytes]u8 {
    var e: [entry_bytes]u8 = @splat(@splat(0));
    e[0] = @intCast(c.k_exfat_entry_stream);
    e[@intCast(c.k_exfat_strm_off_flags)] = if (nofat) @intCast(c.k_exfat_secflag_no_fat) else 0;
    e[@intCast(c.k_exfat_strm_off_nlen)] = nlen;
    wr32(&e, @intCast(c.k_exfat_strm_off_clus), first);
    wr64(&e, @intCast(c.k_exfat_strm_off_dlen), size);
    return e;
}

fn mkName(units: []const u16) [entry_bytes]u8 {
    var e: [entry_bytes]u8 = @splat(@splat(0));
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

export fn priv_sec_io() callconv(.c) [*c]u8 {
    return &sec_buf;
}

export fn priv_bps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32 {
    _ = m;
    return bps;
}

export fn priv_exfat_bmp_switch(m: [*c]const c.ra8_fs_mount_t, lba: u64, loaded: [*c]u64, sec: [*c]u8) callconv(.c) u16 {
    _ = m;
    if (bmp_err != 0) return bmp_err;
    const i: usize = @intCast(lba - bmp_lba);
    @memcpy(sec[0..bps], &bmp_store[i]);
    loaded[0] = lba;
    bmp_calls += 1;
    return ok;
}

export fn priv_write_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]const u8) callconv(.c) u16 {
    _ = m;
    if (write_err != 0) return write_err;
    const i: usize = @intCast(lba - bmp_lba);
    @memcpy(&bmp_store[i], buf[0..bps]);
    last_write_lba = lba;
    write_count += 1;
    return ok;
}

export fn priv_alloc_hint_lower(m: [*c]const c.ra8_fs_mount_t, cluster: u32) callconv(.c) void {
    _ = m;
    hint_calls[hint_count] = cluster;
    hint_count += 1;
}

export fn priv_exfat_bitmap_lba(m: [*c]const c.ra8_fs_mount_t, out_lba: [*c]u64) callconv(.c) u16 {
    _ = m;
    bmp_lba_calls += 1;
    out_lba[0] = bmp_lba;
    return ok;
}

export fn priv_cluster_bytes(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32 {
    _ = m;
    return bps;
}

export fn priv_fat_get(m: [*c]const c.ra8_fs_mount_t, cluster: u32, out_value: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_value[0] = fat_tbl[cluster - first_data];
    return ok;
}

export fn priv_is_eoc(m: [*c]const c.ra8_fs_mount_t, value: u32) callconv(.c) u8 {
    _ = m;
    return @intFromBool(value >= 0xFFFF_FFF8);
}

export fn priv_exfat_next_entry(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.exfat_cursor_t, out: [*c]u8) callconv(.c) u16 {
    _ = m;
    if (script_err != 0) return script_err;
    if (script_idx >= script_len) return not_found;
    @memcpy(out[0..entry_bytes], &script[script_idx]);
    cur[0].entry_in_cluster += 1;
    cur[0].scanned += 1;
    script_idx += 1;
    return ok;
}

export fn priv_exfat_name_chunk_eq(entry: [*c]const u8, name: [*c]const u16, pos: u32, nlen: u32) callconv(.c) u32 {
    var j: u32 = 0;
    while (j < 15) : (j += 1) {
        const idx = pos + j;
        const unit: u16 = @as(u16, entry[2 + j * 2]) | (@as(u16, entry[3 + j * 2]) << 8);
        const want: u16 = if (idx < nlen) name[idx] else 0xFFFF;
        if (unit != want) return 0;
    }
    return 1;
}

export fn priv_exfat_needle_units(m: [*c]const c.ra8_fs_mount_t, name: [*c]const u8, out: [*c]u16, out_units: [*c]u32, too_long: u16) callconv(.c) u16 {
    _ = m;
    var n: u32 = 0;
    while (name[n] != 0) : (n += 1) {}
    if (n > 64) return too_long;
    var i: u32 = 0;
    while (i < n) : (i += 1) out[i] = name[i];
    out_units[0] = n;
    return ok;
}

export fn priv_exfat_cursor_init(dir: [*c]const c.exfat_dir_t, out: [*c]c.exfat_cursor_t) callconv(.c) void {
    _ = dir;
    out[0] = .{ .cluster = dir_cluster, .entry_in_cluster = 0, .scanned = 0, .contig_end = 0 };
}

export fn priv_exfat_write_dir_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32, idx: u32, set: [*c]const u8, bytes: u32) callconv(.c) u16 {
    _ = m;
    _ = bytes;
    wdir_cluster[wdir_count] = cluster;
    wdir_index[wdir_count] = idx;
    wdir_entry0[wdir_count] = set[0];
    wdir_count += 1;
    return ok;
}

export fn priv_exfat_resolve_parent(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out: [*c]c.exfat_dir_t, leaf: [*c][*c]const u8) callconv(.c) u16 {
    _ = m;
    out[0] = std.mem.zeroes(c.exfat_dir_t);
    var i: usize = 0;
    var last: usize = 0;
    while (path[i] != 0) : (i += 1) {
        if (path[i] == '/') last = i;
    }
    leaf[0] = path + last + @intFromBool(path[last] == '/');
    return ok;
}

export fn priv_strlen(s: [*c]const u8) callconv(.c) u32 {
    var n: u32 = 0;
    while (s[n] != 0) : (n += 1) {}
    return n;
}

// ---------------------------------------------------------------- tests

fn dirDummy() c.exfat_dir_t {
    return std.mem.zeroes(c.exfat_dir_t);
}

test "bitmap clear clears bits across bytes and lowers the hint per cluster" {
    reset();
    @memset(&bmp_store[0], 0xFF);
    var m = mkMount();
    const e = mu.priv_exfat_bitmap_clear(&m, bmp_lba, first_data, 9);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 1), write_count);
    try std.testing.expectEqual(bmp_lba, last_write_lba);
    try std.testing.expectEqual(@as(u8, 0x00), bmp_store[0][0]);
    try std.testing.expectEqual(@as(u8, 0xFE), bmp_store[0][1]);
    try std.testing.expectEqual(@as(u32, 9), hint_count);
    var k: u32 = 0;
    while (k < 9) : (k += 1) try std.testing.expectEqual(first_data + k, hint_calls[k]);
}

test "bitmap clear propagates the backend error" {
    reset();
    @memset(&bmp_store[0], 0xFF);
    var m = mkMount();
    bmp_err = c.k_ra8_err_io;
    const e = mu.priv_exfat_bitmap_clear(&m, bmp_lba, first_data, 1);
    try std.testing.expectEqual(c.k_ra8_err_io, e);
    try std.testing.expectEqual(@as(u32, 0), write_count);
}

test "take set consumes secondaries, records positions and matches" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkStream(2, 11, 512, false);
    script[1] = mkName(units[0..2]);
    script_len = 2;
    var cur: c.exfat_cursor_t = .{ .cluster = dir_cluster, .entry_in_cluster = 2, .scanned = 0, .contig_end = 0 };
    var pos: [set_max_entries]c.exfat_setpos_t = std.mem.zeroes([set_max_entries]c.exfat_setpos_t);
    var strm_copy: [entry_bytes]u8 = @splat(@splat(0));
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
    var cur: c.exfat_cursor_t = .{ .cluster = dir_cluster, .entry_in_cluster = 2, .scanned = 0, .contig_end = 0 };
    var pos: [set_max_entries]c.exfat_setpos_t = std.mem.zeroes([set_max_entries]c.exfat_setpos_t);
    var strm_copy: [entry_bytes]u8 = @splat(@splat(0));
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
    var file_copy: [entry_bytes]u8 = @splat(@splat(0));
    var strm_copy: [entry_bytes]u8 = @splat(@splat(0));

    const file_big = mkFile(3, 0);
    script[0] = mkStream(2, 11, 512, false);
    script[1] = mkName(units[0..2]);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    var cur: c.exfat_cursor_t = .{ .cluster = dir_cluster, .entry_in_cluster = 0, .scanned = 0, .contig_end = 0 };
    var e = mu.internal_exfat_try_set(&m0, &cur, pos[0], &file_big, &units, 2, &pos, 2, &count, &file_copy, &strm_copy);
    try std.testing.expectEqual(no_mem, e);

    reset();
    script[0] = mkStream(2, 11, 512, false);
    script[1] = mkName(units[0..2]);
    script_len = 2;
    cur = .{ .cluster = dir_cluster, .entry_in_cluster = 0, .scanned = 0, .contig_end = 0 };
    e = mu.internal_exfat_try_set(&m0, &cur, pos[0], &file_big, &units, 2, &pos, 4, &count, &file_copy, &strm_copy);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 3), count);
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
    var file_copy: [entry_bytes]u8 = @splat(@splat(0));
    var strm_copy: [entry_bytes]u8 = @splat(@splat(0));
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
    var file_copy: [entry_bytes]u8 = @splat(@splat(0));
    var strm_copy: [entry_bytes]u8 = @splat(@splat(0));
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
    var entry: [entry_bytes]u8 = @splat(@splat(0));
    entry[0] = 0x05;
    const e = mu.internal_exfat_put_entry(&m, &where, &entry);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(usize, 1), wdir_count);
    try std.testing.expectEqual(@as(u32, 9), wdir_cluster[0]);
    try std.testing.expectEqual(@as(u32, 4), wdir_index[0]);
    try std.testing.expectEqual(@as(u8, 0x05), wdir_entry0[0]);
}

test "free clusters clears a contiguous run sized from the 64-bit length" {
    reset();
    var m = mkMount();
    const strm = mkStream(2, 5, 1024, true);
    const e = mu.priv_exfat_free_clusters(&m, &strm);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 1), write_count);
    // clusters 5 and 6 -> indices 3 and 4 -> byte 0 bits 3 and 4
    try std.testing.expectEqual(@as(u8, 0xE7), bmp_store[0][0]);
    try std.testing.expectEqual(@as(u32, 2), hint_count);
    try std.testing.expectEqual(@as(u32, 5), hint_calls[0]);
    try std.testing.expectEqual(@as(u32, 6), hint_calls[1]);
}

test "free clusters walks a FAT chain to end-of-chain" {
    reset();
    var m = mkMount();
    const strm = mkStream(2, 5, 1024, false);
    fat_tbl[5 - first_data] = 6;
    fat_tbl[6 - first_data] = eoc_test;
    const e = mu.priv_exfat_free_clusters(&m, &strm);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u32, 1), write_count);
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
    var m = mkMount();
    var pos: [2]c.exfat_setpos_t = .{ .{ .cluster = 9, .index = 4 }, .{ .cluster = 9, .index = 5 } };
    script[0] = mkFile(2, 0);
    script[1] = mkStream(2, 11, 512, false);
    script_len = 2;
    const e = mu.priv_exfat_drop_set(&m, &pos, 2);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(usize, 2), wdir_count);
    try std.testing.expectEqual(@as(u32, 9), wdir_cluster[0]);
    try std.testing.expectEqual(@as(u32, 4), wdir_index[0]);
    try std.testing.expectEqual(@as(u32, 5), wdir_index[1]);
    try std.testing.expectEqual(@as(u8, 0x05), wdir_entry0[0]);
    try std.testing.expectEqual(@as(u8, 0x40), wdir_entry0[1]);
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
    try std.testing.expectEqual(@as(usize, 0), wdir_count);
    try std.testing.expectEqual(@as(u32, 0), write_count);

    reset();
    script[0] = mkFile(2, @intCast(c.k_exfat_attr_read_only));
    script[1] = mkStream(2, 11, 512, false);
    script[2] = mkName(mkUnits("AB")[0..2]);
    script_len = 3;
    const e2 = mu.priv_exfat_unlink_at(&m, &dir, "AB");
    try std.testing.expectEqual(access_denied, e2);
    try std.testing.expectEqual(@as(usize, 0), wdir_count);
}

test "unlink at drops the set and frees the clusters" {
    reset();
    var m = mkMount();
    const dir = dirDummy();
    const units = mkUnits("AB");
    script[0] = mkFile(2, 0x20);
    script[1] = mkStream(2, 5, 1024, true);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    const e = mu.priv_exfat_unlink_at(&m, &dir, "AB");
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(usize, 3), wdir_count);
    try std.testing.expectEqual(@as(u32, 1), write_count);
    try std.testing.expectEqual(@as(u8, 0xE7), bmp_store[0][0]);
}

test "unlink refuses an empty leaf and unlinks a file by path" {
    reset();
    var m = mkMount();
    const e = mu.priv_exfat_unlink(&m, "/");
    try std.testing.expectEqual(invalid_arg, e);

    reset();
    const units = mkUnits("AB");
    script[0] = mkFile(2, 0x20);
    script[1] = mkStream(2, 5, 1024, true);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    const e2 = mu.priv_exfat_unlink(&m, "/dir/AB");
    try std.testing.expectEqual(ok, e2);
    try std.testing.expectEqual(@as(usize, 3), wdir_count);
}

var m0: c.ra8_fs_mount_t = std.mem.zeroes(c.ra8_fs_mount_t);
