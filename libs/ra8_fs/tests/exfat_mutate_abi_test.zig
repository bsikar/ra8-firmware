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

fn mkGarbage(t: u8) [entry_bytes]u8 {
    var e: [entry_bytes]u8 = @splat(@splat(0));
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

export fn priv_exfat_find(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, path: [*c]const u8, out_strm: [*c]u8, out_attr: [*c]u8) callconv(.c) u16 {
    _ = m;
    _ = dir;
    _ = path;
    _ = out_strm;
    _ = out_attr;
    return find_err;
}

export fn priv_exfat_find_dir_space(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, need: u32, out_clus: [*c]u32, out_idx: [*c]u32) callconv(.c) u16 {
    _ = m;
    _ = dir;
    _ = need;
    if (space_err != 0) return space_err;
    out_clus[0] = space_clus;
    out_idx[0] = space_idx;
    return ok;
}

export fn priv_exfat_name_hash(name: [*c]const u16, nlen: u32) callconv(.c) u16 {
    _ = name;
    hash_calls += 1;
    return @truncate(nlen *% 7 +% 1);
}

export fn priv_exfat_file_stamp_access(file_entry: [*c]u8) callconv(.c) void {
    _ = file_entry;
    stamp_calls += 1;
}

export fn priv_exfat_set_checksum(set: [*c]const u8, bytes: u32) callconv(.c) u16 {
    _ = set;
    _ = bytes;
    csum_calls += 1;
    return 0xBEEF;
}

export fn priv_wr16(p: [*c]u8, v: u16) callconv(.c) void {
    p[0] = @truncate(v);
    p[1] = @truncate(v >> 8);
}

export fn priv_utf16_to_utf8(in: [*c]const u16, units: u32, out: [*c]u8, cap: u32) callconv(.c) u16 {
    _ = cap;
    if (utf_err != 0) return utf_err;
    var n: u32 = 0;
    var i: u32 = 0;
    while (i < units) : (i += 1) {
        if (in[i] >= 0x80) return invalid_arg;
        out[n] = @truncate(in[i]);
        n += 1;
    }
    out[n] = 0;
    return ok;
}

fn listCb(name: [*c]const u8, attr: u8, size: u64, ctx: ?*anyopaque) callconv(.c) void {
    _ = attr;
    _ = size;
    _ = ctx;
    var i: usize = 0;
    while (name[i] != 0) : (i += 1) {}
    @memcpy(&cb_names[cb_n][0..i], name[0..i]);
    cb_n += 1;
}

// ------------------------------------------------------------ rename tests

test "build rename set keeps every non-name field and pads to entry boundaries" {
    reset();
    var file_e = mkFile(2, 0x20);
    file_e[12] = 0xAB; // create-stamp bytes ride across untouched
    file_e[16] = 0xCD; // modify-stamp bytes ride across untouched
    var strm_e = mkStream(2, 11, 512, false);
    strm_e[2] = 7; // stale SecondaryCount is overwritten
    const units = mkUnits("XY");
    var set: [c.k_exfat_max_set_bytes]u8 = @splat(@splat(0));
    const n = mu.internal_exfat_build_rename_set(&set, &file_e, &strm_e, &units, 2);
    try std.testing.expectEqual(@as(u32, 96), n);
    try std.testing.expectEqual(@as(u8, 2), set[1]); // File type byte untouched
    try std.testing.expectEqual(@as(u8, 2), set[2]); // SecondaryCount = 1 + 1
    try std.testing.expectEqual(file_e[12], set[12]);
    try std.testing.expectEqual(file_e[16], set[16]);
    try std.testing.expectEqual(@as(u8, 2), set[entry_bytes + strm_nlen_i]);
    try std.testing.expectEqual(@as(u16, 15), rd16v(&set, entry_bytes + strm_hash_i)); // 2*7+1
    try std.testing.expectEqual(entry_name_b, set[2 * entry_bytes]);
    try std.testing.expectEqual(@as(u8, 'X'), set[2 * entry_bytes + name_off_i]);
    try std.testing.expectEqual(@as(u8, 0), set[2 * entry_bytes + name_off_i + 1]);
    try std.testing.expectEqual(@as(u8, 'Y'), set[2 * entry_bytes + name_off_i + 2]);
    try std.testing.expectEqual(@as(u8, 0), set[2 * entry_bytes + name_off_i + 4]);
    try std.testing.expectEqual(@as(u32, 1), stamp_calls);
    try std.testing.expectEqual(@as(u32, 1), csum_calls);
    try std.testing.expectEqual(@as(u8, 0xEF), set[file_csum_i]);
    try std.testing.expectEqual(@as(u8, 0xBE), set[file_csum_i + 1]);
}

test "build rename set grows a second name entry with zero padding" {
    reset();
    var file_e = mkFile(2, 0x20);
    var strm_e = mkStream(2, 11, 512, false);
    const units = mkUnits19("ABCDEFGHIJKLMNOPQRS"); // 19 units
    var set: [c.k_exfat_max_set_bytes]u8 = @splat(@splat(0));
    const n = mu.internal_exfat_build_rename_set(&set, &file_e, &strm_e, &units, 19);
    try std.testing.expectEqual(@as(u32, 128), n);
    try std.testing.expectEqual(@as(u8, 3), set[2]); // SecondaryCount = 1 + 2
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
    var parent: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var ol: [*c]const u8 = null;
    var nl: [*c]const u8 = null;
    const e = mu.internal_exfat_rename_prepare(&m0, "/d/AB", "#/XY", &parent, &ol, &nl);
    try std.testing.expectEqual(not_supported, e);

    const e2 = mu.internal_exfat_rename_prepare(&m0, "/d/AB", "/d/", &parent, &ol, &nl);
    try std.testing.expectEqual(invalid_arg, e2);
}

test "place rename rewrites every slot when the entry count is unchanged" {
    reset();
    var pos: [3]c.exfat_setpos_t = undefined;
    pos[0] = .{ .cluster = 1, .index = 0 };
    pos[1] = .{ .cluster = 1, .index = 1 };
    pos[2] = .{ .cluster = 1, .index = 2 };
    var set: [96]u8 = @splat(@splat(0));
    set[0] = entry_file_b;
    set[32] = entry_stream_b;
    set[64] = entry_name_b;
    const dir = dirDummy();
    const e = mu.internal_exfat_place_rename(&m0, &dir, &pos, 3, &set, 96);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(usize, 3), wdir_count);
    var k: usize = 0;
    while (k < 3) : (k += 1) {
        try std.testing.expectEqual(@as(u32, 1), wdir_cluster[k]);
        try std.testing.expectEqual(@as(u32, @intCast(k)), wdir_index[k]);
    }
    try std.testing.expectEqual(entry_file_b, wdir_entry0[0]);
    try std.testing.expectEqual(entry_stream_b, wdir_entry0[1]);
    try std.testing.expectEqual(entry_name_b, wdir_entry0[2]);
}

test "place rename relocates a longer set, then retires the old one" {
    reset();
    space_clus = 5;
    space_idx = 3;
    const units = mkUnits("AB");
    script[0] = mkFile(2, 0x20);
    script[1] = mkStream(2, 5, 1024, true);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    var pos: [3]c.exfat_setpos_t = undefined;
    pos[0] = .{ .cluster = 1, .index = 0 };
    pos[1] = .{ .cluster = 1, .index = 1 };
    pos[2] = .{ .cluster = 1, .index = 2 };
    var set: [128]u8 = @splat(@splat(0));
    set[0] = 0x99;
    const dir = dirDummy();
    const e = mu.internal_exfat_place_rename(&m0, &dir, &pos, 3, &set, 128);
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(usize, 4), wdir_count);
    try std.testing.expectEqual(@as(u32, 5), wdir_cluster[0]); // the new home
    try std.testing.expectEqual(@as(u32, 3), wdir_index[0]);
    try std.testing.expectEqual(@as(u8, 0x99), wdir_entry0[0]);
    const dropped: [3]u8 = .{ entry_file_b, entry_stream_b, entry_name_b };
    k = 0;
    while (k < 3) : (k += 1) {
        try std.testing.expectEqual(dropped[k] & ~inuse, wdir_entry0[k + 1]);
    }
}

test "place rename propagates the free-slot failure" {
    reset();
    space_err = c.k_ra8_err_io;
    var pos: [3]c.exfat_setpos_t = undefined;
    pos[0] = .{ .cluster = 1, .index = 0 };
    pos[1] = .{ .cluster = 1, .index = 1 };
    pos[2] = .{ .cluster = 1, .index = 2 };
    var set: [128]u8 = @splat(@splat(0));
    const dir = dirDummy();
    const e = mu.internal_exfat_place_rename(&m0, &dir, &pos, 3, &set, 128);
    try std.testing.expectEqual(c.k_ra8_err_io, e);
    try std.testing.expectEqual(@as(usize, 0), wdir_count);
}

test "rename refuses an existing new name before anything is written" {
    reset();
    find_err = ok; // the new name already exists
    const e = mu.priv_exfat_rename(&m0, "/d/AB", "/d/XY");
    try std.testing.expectEqual(exists, e);
    try std.testing.expectEqual(@as(usize, 0), wdir_count);
}

test "rename refuses different parents" {
    reset();
    const e = mu.priv_exfat_rename(&m0, "/d/AB", "#/XY");
    try std.testing.expectEqual(not_supported, e);
    try std.testing.expectEqual(@as(usize, 0), wdir_count);
}

test "rename honors the read-only attribute" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkFile(2, @intCast(c.k_exfat_attr_read_only));
    script[1] = mkStream(2, 5, 1024, false);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    const e = mu.priv_exfat_rename(&m0, "/d/AB", "/d/XY");
    try std.testing.expectEqual(access_denied, e);
    try std.testing.expectEqual(@as(usize, 0), wdir_count);
}

test "rename rebuilds the set and places it when the count is unchanged" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkFile(2, 0x20);
    script[1] = mkStream(2, 5, 1024, false);
    script[2] = mkName(units[0..2]);
    script_len = 3;
    const e = mu.priv_exfat_rename(&m0, "/d/AB", "/d/XY");
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(usize, 3), wdir_count);
    try std.testing.expectEqual(@as(u32, 1), hash_calls);
    try std.testing.expectEqual(@as(u32, 1), stamp_calls);
    try std.testing.expectEqual(@as(u32, 1), csum_calls);
    try std.testing.expectEqual(entry_file_b, wdir_entry0[0]);
    try std.testing.expectEqual(entry_stream_b, wdir_entry0[1]);
    try std.testing.expectEqual(entry_name_b, wdir_entry0[2]);
}

// --------------------------------------------------------- listdir tests

test "gather name consumes secondaries and converts the units once" {
    reset();
    const units = mkUnits("AB");
    script[0] = mkName(units[0..2]);
    script[1] = mkGarbage(0x99); // a non-Name secondary is skipped
    script_len = 2;
    var cur = freshCursor(2);
    var name: [name_u8_cap]u8 = @splat(@splat(0));
    const e = mu.internal_exfat_gather_name(&m0, &cur, 3, 2, &name, @intCast(name_u8_cap));
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u8, 'A'), name[0]);
    try std.testing.expectEqual(@as(u8, 'B'), name[1]);
    try std.testing.expectEqual(@as(u8, 0), name[2]);
    try std.testing.expectEqual(@as(u32, 2), cur.scanned); // both secondaries consumed
}

test "gather name yields nothing for units no UTF-8 string encodes" {
    reset();
    var ne = mkName(mkUnits("X")[0..1]);
    wr16(&ne, name_off_i, 0x00E9); // a lone accented unit: the fake refuses it
    script[0] = ne;
    script_len = 1;
    var cur = freshCursor(0);
    var name: [name_u8_cap]u8 = @splat(@splat(0));
    const e = mu.internal_exfat_gather_name(&m0, &cur, 2, 1, &name, @intCast(name_u8_cap));
    try std.testing.expectEqual(ok, e);
    try std.testing.expectEqual(@as(u8, 0), name[0]);
}

test "gather name propagates a backend failure mid-set" {
    reset();
    script_err = c.k_ra8_err_io;
    var cur = freshCursor(0);
    var name: [name_u8_cap]u8 = @splat(@splat(0));
    const e = mu.internal_exfat_gather_name(&m0, &cur, 3, 2, &name, @intCast(name_u8_cap));
    try std.testing.expectEqual(c.k_ra8_err_io, e);
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
    wr16(&ne, name_off_i, 0x4F60);
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
