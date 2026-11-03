//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT read side (RA8FW-1087): volume parse, the up-case walk, the
//! directory cursor machinery, flat name matching, and the read open,
//! against this file's own fakes.

const std = @import("std");
const fs = @import("ra8_fs");
const ex = fs.exfat_read;
const exw = fs.exfat_write;
const upcase = fs.upcase;
const c = fs.c;

const bps: u32 = 512;
const spc: u32 = 1;
const cluster_bytes: u32 = bps * spc;
const per_cluster: u32 = cluster_bytes / 32;
const data_lba: u64 = 1000;
const eoc: u32 = 0x0FFFFFF8;
const disk_sectors: usize = 8;

const ok: u16 = c.k_ra8_ok;
const err_not_supported: u16 = c.k_ra8_err_not_supported;
const err_not_found: u16 = c.k_ra8_err_not_found;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_no_mem: u16 = c.k_ra8_err_no_mem;

// ---------------------------------------------------------------- fakes

export var g_fs_scratch: [c.k_ra8_fs_sector_max]u8 = @splat(0);

var disk: [disk_sectors][bps]u8 = @splat(@splat(0));
var read_err: u16 = 0;
var fat: [8]u32 = @splat(0);
var fat_read_err: u16 = 0;
var bpb_result: u16 = 0xAAAA;
var pool: [2]c.ra8_fs_file_t = undefined;
var pool_used: usize = 0;
var lookup_err: u16 = 0;
var lookup_strm: [32]u8 = @splat(0);
var lookup_attr: u8 = 0;

fn reset() void {
    for (&disk) |*s| s.* = @splat(0);
    for (&fat) |*f| f.* = 0;
    read_err = 0;
    write_err = 0;
    fat_read_err = 0;
    pool_used = 0;
    bpb_result = 0xAAAA;
    lookup_err = 0;
    lookup_strm = @splat(0);
    lookup_attr = 0;
    grow_calls = 0;
    @memset(&g_fs_scratch, 0);
}

export fn priv_cluster_to_lba(m: [*c]const c.ra8_fs_mount_t, cluster: u32) callconv(.c) u64 {
    return m.*.first_data_lba + (cluster - 2) * m.*.sectors_per_cluster;
}

export fn priv_fat_get(m: [*c]const c.ra8_fs_mount_t, cluster: u32, out: *u32) callconv(.c) u16 {
    _ = m;
    if (fat_read_err != 0) return fat_read_err;
    out.* = fat[cluster - 2];
    return ok;
}

var sec_buf: [c.k_ra8_fs_sector_max]u8 = @splat(0);

export fn priv_sec_io() callconv(.c) [*c]u8 {
    return &sec_buf;
}

fn read_block_backend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (read_err != 0) return read_err;
    const i: usize = @intCast(lba - data_lba);
    @memcpy(buf[0..bps], &disk[i]);
    return ok;
}

export fn priv_alloc_file_slot() callconv(.c) ?*ex.File {
    if (pool_used >= pool.len) return null;
    const f = &pool[pool_used];
    pool_used += 1;
    f.* = std.mem.zeroes(c.ra8_fs_file_t);
    return f;
}

export fn priv_parse_bpb_into_mount(m: [*c]c.ra8_fs_mount_t) callconv(.c) u16 {
    _ = m;
    return bpb_result;
}

export fn priv_exfat_lookup(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out_strm: [*c]u8, out_attr: [*c]u8) callconv(.c) u16 {
    _ = m;
    _ = path;
    @memcpy(out_strm[0..32], &lookup_strm);
    out_attr.* = lookup_attr;
    return lookup_err;
}

// Openw's callees, analyzed when priv_exfat_open's routing branch compiles;
// none of them runs on the read path under test.
export fn priv_exfat_free_clusters(m: [*c]const c.ra8_fs_mount_t, strm: [*c]const u8) callconv(.c) u16 {
    _ = m;
    _ = strm;
    return ok;
}

export fn priv_exfat_flush_set(f: [*c]c.ra8_fs_file_t) callconv(.c) u16 {
    _ = f;
    return ok;
}

var resolve_err: u16 = 0;

export fn priv_exfat_resolve_parent(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out: [*c]c.exfat_dir_t, leaf: [*c][*c]const u8) callconv(.c) u16 {
    _ = m;
    _ = out;
    leaf.* = path;
    return resolve_err;
}

// --------------------------------------------------------------- helpers

fn mount() c.ra8_fs_mount_t {
    var m = std.mem.zeroes(c.ra8_fs_mount_t);
    m.type = c.k_ra8_fs_type_exfat;
    m.bytes_per_sector = bps;
    m.backend.read_block = read_block_backend;
    m.backend.write_block = write_block_backend;
    m.sectors_per_cluster = spc;
    m.first_data_lba = data_lba;
    m.root_cluster = 2;
    return m;
}

fn wr32(sector: [*]u8, off: usize, v: u32) void {
    sector[off] = @truncate(v);
    sector[off + 1] = @truncate(v >> 8);
    sector[off + 2] = @truncate(v >> 16);
    sector[off + 3] = @truncate(v >> 24);
}

fn wr64(sector: [*]u8, off: usize, v: u64) void {
    wr32(sector, off, @truncate(v));
    wr32(sector, off + 4, @truncate(v >> 32));
}

fn strEntry(sector: *[bps]u8, idx: usize, cluster: u32, flags: u8, dlen: u64, valid: u64) void {
    const base = idx * 32;
    sector[base] = 0xC0;
    sector[base + 1] = flags;
    sector[base + 3] = 9; // NameLength
    wr64(sector, base + 8, valid);
    wr32(sector, base + 0x14, cluster);
    wr64(sector, base + 0x18, dlen);
}

fn nameEntry(sector: *[bps]u8, idx: usize, name: []const u8) void {
    const base = idx * 32;
    sector[base] = 0xC1;
    for (name, 0..) |ch, i| sector[base + 2 + i * 2] = ch;
}

fn needleFor(comptime name: []const u8) [c.k_exfat_name_cap]u16 {
    var units: [c.k_exfat_name_cap]u16 = @splat(0);
    for (name, 0..) |ch, i| units[i] = ch;
    return units;
}

test "the shared string helpers survived the port" {
    reset();
    try std.testing.expectEqual(@as(u32, 9), ex.priv_strlen("notes.txt"));
    try std.testing.expectEqual(@as(u32, 0), ex.priv_strlen(""));
    try std.testing.expectEqual(@as(c_char, 'C'), ex.priv_ascii_upper('c'));
    try std.testing.expectEqual(@as(c_char, '3'), ex.priv_ascii_upper('3'));
}

// Link stubs for the other root units this binary analyzes (never exercised
// here): the shared root pulls every abi module into every test root.
var walk_sector: [512]u8 = @splat(0);

export fn priv_dir_find(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, name83: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32, out_entry: [*c]u8) callconv(.c) u16 {
    _ = m;
    _ = loc;
    _ = name83;
    _ = out_lba;
    _ = out_off;
    _ = out_entry;
    return err_not_found;
}

export fn priv_dir_walk_init_loc(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, w: [*c]c.dir_walk_t) callconv(.c) void {
    w.* = std.mem.zeroes(c.dir_walk_t);
    _ = m;
    _ = loc;
}

export fn priv_dir_walk_next_sector(m: [*c]const c.ra8_fs_mount_t, w: [*c]c.dir_walk_t, out_eod: [*c]u8) callconv(.c) u16 {
    _ = m;
    _ = w;
    out_eod.* = 1;
    return 0;
}

export fn priv_exfat_csum32(cs: u32, buf: [*c]const u8, len: u32) callconv(.c) u32 {
    var sum = cs;
    for (buf[0..len]) |b| sum = std.math.rotr(u32, sum, 1) +% b;
    return sum;
}

export fn priv_exfat_file_set_times(entry: [*c]u8, create: [*c]const c.ra8_fs_datetime_t, modify: [*c]const c.ra8_fs_datetime_t, access: [*c]const c.ra8_fs_datetime_t) callconv(.c) void {
    _ = .{ entry, create, modify, access };
}

export fn priv_exfat_find_set(m: [*c]const c.ra8_fs_mount_t, d: [*c]const c.exfat_dir_t, path: [*c]const u8, pos: [*c]c.exfat_setpos_t, max_pos: u32, out_count: [*c]u32, file_copy: [*c]u8, strm_copy: [*c]u8) callconv(.c) u16 {
    _ = m;
    _ = d;
    _ = path;
    _ = pos;
    _ = max_pos;
    _ = out_count;
    _ = file_copy;
    _ = strm_copy;
    return err_not_found;
}

export fn priv_fat_entry_set_times(entry: [*c]u8, create: [*c]const c.ra8_fs_datetime_t, modify: [*c]const c.ra8_fs_datetime_t, access: [*c]const c.ra8_fs_datetime_t) callconv(.c) void {
    _ = .{ entry, create, modify, access };
}

export fn priv_fat_set(m: [*c]const c.ra8_fs_mount_t, clus: u32, value: u32) callconv(.c) u16 {
    _ = m;
    _ = clus;
    _ = value;
    return 0;
}

export fn priv_path_to_83(path: [*c]const u8, out11: [*c]u8) callconv(.c) u8 {
    _ = path;
    _ = out11;
    return 0;
}

export fn priv_sec_walk() callconv(.c) [*c]u8 {
    return &walk_sector;
}

var write_err: u16 = 0;

fn write_block_backend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (write_err != 0) return write_err;
    const i: usize = @intCast(lba - data_lba);
    @memcpy(&disk[i], buf[0..bps]);
    return ok;
}
var grow_calls: u32 = 0;

export fn priv_exfat_grow_dir(m: [*c]const c.ra8_fs_mount_t, dir: [*c]c.exfat_dir_t) callconv(.c) u16 {
    _ = m;
    grow_calls += 1;
    dir.*.contig_end = dir.*.cluster + 2;
    return ok;
}

export fn priv_exfat_file_stamp_create(entry: [*c]u8) callconv(.c) void {
    entry[7] = 0x42;
}

fn foldRef(cs: u16, b: u8) u16 {
    const hi: u16 = if ((cs & 1) != 0) 0x8000 else 0;
    return hi +% (cs >> 1) +% @as(u16, b);
}

// ---------------------------------------------------------------- tests

test "name hash folds the upcased name one byte at a time" {
    reset();
    const upper = needleFor("ABC");
    try std.testing.expectEqual(@as(u16, 0xC82B), exw.priv_exfat_name_hash(&upper, 3));
    const lower = needleFor("abc");
    try std.testing.expectEqual(@as(u16, 0xC82B), exw.priv_exfat_name_hash(&lower, 3));
}

test "set checksum skips its own two bytes" {
    reset();
    var set: [32]u8 = undefined;
    for (&set, 0..) |*b, i| b.* = @truncate(i * 7 + 1);
    const cs1 = exw.priv_exfat_set_checksum(&set, 32);
    set[2] = 0xA5;
    set[3] = 0x5A;
    try std.testing.expectEqual(cs1, exw.priv_exfat_set_checksum(&set, 32));
    var ref: u16 = 0;
    for (0..32) |i| {
        if (i == 2 or i == 3) continue;
        ref = foldRef(ref, set[i]);
    }
    try std.testing.expectEqual(ref, cs1);
}

test "find_bitmap locates the bitmap entry in the root" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    disk[0][0] = 0x85;
    disk[0][32] = 0x81;
    wr32(&disk[0], 32 + 0x14, 5);
    wr32(&disk[0], 32 + 0x18, 1024);
    var clus: u32 = 0;
    var len: u32 = 0;
    try std.testing.expectEqual(ok, exw.priv_exfat_find_bitmap(&m, &clus, &len));
    try std.testing.expectEqual(@as(u32, 5), clus);
    try std.testing.expectEqual(@as(u32, 1024), len);
}

test "find_bitmap reports not_found at end-of-directory" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    var clus: u32 = 0;
    var len: u32 = 0;
    try std.testing.expectEqual(err_not_found, exw.priv_exfat_find_bitmap(&m, &clus, &len));
}

test "find_bitmap propagates a backend read failure" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    read_err = 0x1010;
    var clus: u32 = 0;
    var len: u32 = 0;
    try std.testing.expectEqual(@as(u16, 0x1010), exw.priv_exfat_find_bitmap(&m, &clus, &len));
}

test "bmp switch keeps one sector resident and writes the old one back" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    var loaded: u64 = std.math.maxInt(u64);
    disk[3][5] = 0x33;
    try std.testing.expectEqual(ok, exw.priv_exfat_bmp_switch(&m, data_lba + 3, &loaded, &sec_buf));
    try std.testing.expectEqual(data_lba + 3, loaded);
    try std.testing.expectEqual(@as(u8, 0x33), sec_buf[5]);
    try std.testing.expectEqual(ok, exw.priv_exfat_bmp_switch(&m, data_lba + 3, &loaded, &sec_buf));
    sec_buf[5] = 0x44;
    try std.testing.expectEqual(ok, exw.priv_exfat_bmp_switch(&m, data_lba + 4, &loaded, &sec_buf));
    try std.testing.expectEqual(@as(u8, 0x44), disk[3][5]);
    try std.testing.expectEqual(disk[4][5], sec_buf[5]);
}

test "bitmap mark sets bits across bytes and flushes" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    try std.testing.expectEqual(ok, exw.priv_exfat_bitmap_mark(&m, data_lba + 3, 2, 10));
    try std.testing.expectEqual(@as(u8, 0xFF), disk[3][0]);
    try std.testing.expectEqual(@as(u8, 0x03), disk[3][1]);
}

test "bitmap mark propagates a backend read failure" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    read_err = 0x1011;
    try std.testing.expectEqual(@as(u16, 0x1011), exw.priv_exfat_bitmap_mark(&m, data_lba + 3, 2, 4));
}

test "bitmap test reads one bit as free or busy" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    disk[3][0] = 0x01;
    var free: u8 = 9;
    try std.testing.expectEqual(ok, exw.priv_exfat_bitmap_test(&m, data_lba + 3, 2, &free));
    try std.testing.expectEqual(@as(u8, 0), free);
    try std.testing.expectEqual(ok, exw.priv_exfat_bitmap_test(&m, data_lba + 3, 3, &free));
    try std.testing.expectEqual(@as(u8, 1), free);
}

test "bitmap scan starts at the hint and falls back to a full rescan" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    disk[3][0] = 0xFF;
    var clus: u32 = 0;
    try std.testing.expectEqual(ok, exw.priv_exfat_bitmap_scan(&m, data_lba + 3, 2, &clus));
    try std.testing.expectEqual(@as(u32, 10), clus);
    disk[3][0] = 0;
    try std.testing.expectEqual(ok, exw.priv_exfat_bitmap_scan(&m, data_lba + 3, 2, &clus));
    try std.testing.expectEqual(@as(u32, 2), clus);
}

test "find_dir_space finds a run inside the current cluster" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    disk[0][0] = 0x85;
    disk[0][32] = 0xC0;
    disk[0][64] = 0x86;
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    dir.contig_end = 2;
    var clus: u32 = 0;
    var idx: u32 = 0;
    try std.testing.expectEqual(ok, exw.priv_exfat_find_dir_space(&m, &dir, 4, &clus, &idx));
    try std.testing.expectEqual(@as(u32, 2), clus);
    try std.testing.expectEqual(@as(u32, 3), idx);
}

test "find_dir_space grows the directory when the current run is full" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    for (0..16) |i| disk[0][i * 32] = 0x85;
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    dir.contig_end = 2;
    var clus: u32 = 0;
    var idx: u32 = 0;
    try std.testing.expectEqual(ok, exw.priv_exfat_find_dir_space(&m, &dir, 3, &clus, &idx));
    try std.testing.expectEqual(@as(u32, 3), clus);
    try std.testing.expectEqual(@as(u32, 0), idx);
    try std.testing.expectEqual(@as(u32, 1), grow_calls);
}

test "find_dir_space stops after its one-grow budget" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    for (0..16) |i| {
        disk[0][i * 32] = 0x85;
        disk[1][i * 32] = 0x85;
    }
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    dir.contig_end = 4;
    var clus: u32 = 0;
    var idx: u32 = 0;
    try std.testing.expectEqual(err_no_mem, exw.priv_exfat_find_dir_space(&m, &dir, 3, &clus, &idx));
    try std.testing.expectEqual(@as(u32, 2), grow_calls);
}

test "link builds the set and writes it into directory space" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    disk[0][0] = 0x85;
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    dir.contig_end = 2;
    var units: [c.k_exfat_name_cap]u16 = @splat(0);
    const name = "hi.txt";
    for (name, 0..) |ch, i| units[i] = ch;
    var head: c.exfat_setpos_t = std.mem.zeroes(c.exfat_setpos_t);
    var count: u32 = 0;
    try std.testing.expectEqual(ok, exw.priv_exfat_link(&m, &dir, &units, 6, &head, &count));
    try std.testing.expectEqual(@as(u32, 2), head.cluster);
    try std.testing.expectEqual(@as(u32, 1), head.index);
    try std.testing.expectEqual(@as(u32, 3), count);
    try std.testing.expectEqual(@as(u8, 0x85), disk[0][32]);
    try std.testing.expectEqual(@as(u8, 2), disk[0][33]);
    try std.testing.expectEqual(@as(u8, 0x20), disk[0][36]);
    try std.testing.expectEqual(@as(u8, 0x42), disk[0][39]);
    try std.testing.expectEqual(@as(u8, 0xC0), disk[0][64]);
    try std.testing.expectEqual(@as(u8, 0x01), disk[0][65]);
    try std.testing.expectEqual(@as(u8, 6), disk[0][67]);
    try std.testing.expectEqual(@as(u8, 0xC1), disk[0][96]);
    try std.testing.expectEqual(@as(u8, 'h'), disk[0][98]);
    try std.testing.expectEqual(@as(u8, 'i'), disk[0][100]);
    var ref: u16 = 0;
    for (0..96) |i| {
        if (i == 2 or i == 3) continue;
        ref = foldRef(ref, disk[0][32 + i]);
    }
    try std.testing.expectEqual(@as(u8, @truncate(ref)), disk[0][34]);
    try std.testing.expectEqual(@as(u8, @truncate(ref >> 8)), disk[0][35]);
}

test "link writes every name unit and pads the rest" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    dir.contig_end = 2;
    var units: [c.k_exfat_name_cap]u16 = @splat(0);
    const name = "abcdefghijklmnopqrst";
    for (name, 0..) |ch, i| units[i] = ch;
    var head: c.exfat_setpos_t = std.mem.zeroes(c.exfat_setpos_t);
    var count: u32 = 0;
    try std.testing.expectEqual(ok, exw.priv_exfat_link(&m, &dir, &units, 20, &head, &count));
    try std.testing.expectEqual(@as(u32, 4), count);
    try std.testing.expectEqual(@as(u8, 3), disk[0][1]);
    try std.testing.expectEqual(@as(u8, 'o'), disk[0][64 + 2 + 14 * 2]);
    try std.testing.expectEqual(@as(u8, 'p'), disk[0][96 + 2]);
    try std.testing.expectEqual(@as(u8, 't'), disk[0][96 + 2 + 4 * 2]);
    try std.testing.expectEqual(@as(u8, 0), disk[0][96 + 2 + 6 * 2]);
    try std.testing.expectEqual(@as(u8, 0xC1), disk[0][96]);
}

test "write_dir_set continues across the sector boundary" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    var set: [96]u8 = undefined;
    for (&set, 0..) |*b, i| b.* = @truncate(i + 1);
    try std.testing.expectEqual(ok, exw.priv_exfat_write_dir_set(&m, 2, 14, &set, 96));
    try std.testing.expectEqual(@as(u8, 1), disk[0][448]);
    try std.testing.expectEqual(@as(u8, 49), disk[0][496]);
    try std.testing.expectEqual(@as(u8, 65), disk[1][0]);
    try std.testing.expectEqual(@as(u8, 96), disk[1][31]);
}

test "link grows the directory when its run is full" {
    reset();
    var m = mount();
    m.count_of_clusters = 64;
    for (0..16) |i| disk[0][i * 32] = 0x85;
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    dir.contig_end = 2;
    var units: [c.k_exfat_name_cap]u16 = @splat(0);
    const name = "notes.txt";
    for (name, 0..) |ch, i| units[i] = ch;
    var head: c.exfat_setpos_t = std.mem.zeroes(c.exfat_setpos_t);
    var count: u32 = 0;
    try std.testing.expectEqual(ok, exw.priv_exfat_link(&m, &dir, &units, 9, &head, &count));
    try std.testing.expectEqual(@as(u32, 3), head.cluster);
    try std.testing.expectEqual(@as(u32, 0), head.index);
    try std.testing.expectEqual(@as(u32, 3), count);
    try std.testing.expectEqual(@as(u32, 1), grow_calls);
    try std.testing.expectEqual(@as(u8, 0x85), disk[1][0]);
    try std.testing.expectEqual(@as(u8, 2), disk[1][1]);
    try std.testing.expectEqual(@as(u8, 9), disk[1][35]);
}

// ---- stand-ins for the five C helpers the open/close port (RA8FW-1092) added;
// the open/create path is not exercised here, so they only satisfy the linker
export fn priv_dir_lookup_any(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, leaf: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32, out_entry: [*c]u8) callconv(.c) u16 {
    _ = m;
    _ = loc;
    _ = leaf;
    out_lba.* = 0;
    out_off.* = 0;
    out_entry[0..32].* = @splat(0);
    return 0;
}

export fn priv_dir_reserve(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, leaf: [*c]const u8, plan: [*c]c.dir_insert_t) callconv(.c) u16 {
    _ = m;
    _ = loc;
    _ = leaf;
    _ = plan;
    return 0;
}

export fn priv_dir_commit(m: [*c]const c.ra8_fs_mount_t, plan: [*c]const c.dir_insert_t, tmpl: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32) callconv(.c) u16 {
    _ = m;
    _ = plan;
    _ = tmpl;
    out_lba.* = 0;
    out_off.* = 0;
    return 0;
}

export fn priv_fat_entry_stamp_write(entry: [*c]u8) callconv(.c) void {
    _ = entry;
}

export fn priv_fat_entry_stamp_create(entry: [*c]u8) callconv(.c) void {
    _ = entry;
}

/// FAT label-field writer: pads to the 11-byte field with spaces, or writes
/// the "NO NAME    " sentinel for a null/empty label. Mirrors the C fmt unit.
export fn priv_fmt_label_field(dst: [*c]u8, lbl: [*c]const u8) callconv(.c) void {
    if (lbl == null or lbl[0] == 0) {
        @memcpy(dst[0..11], "NO NAME    ");
        return;
    }
    var i: usize = 0;
    while (i < 11 and lbl[i] != 0) : (i += 1) dst[i] = lbl[i];
    while (i < 11) : (i += 1) dst[i] = ' ';
}
