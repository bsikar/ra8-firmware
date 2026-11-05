//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT cluster growth (RA8FW-1118): cluster pick, run materialization,
//! NoFatChain linking, one-cluster growth, and the ensure loop, against a
//! small disk-backed backend with a real FAT and bitmap window.

const std = @import("std");
const fs = @import("ra8_fs");
const st = fs.exfat_stream;
const al = fs.alloc;
const c = fs.c;

const bps: u32 = 512;
const spc: u32 = 1;
const data_lba: u64 = 1000;
const disk_sectors: usize = 8;
const fat_lba: u64 = 900;
const fat_secs: u64 = 2;
const bmp_lba_want: u64 = 1002;

const ok: u16 = c.k_ra8_ok;
const eoc: u32 = c.k_cluster_eoc_write_exfat;
const first_data: u32 = @intCast(c.k_cluster_first_data);

// ---------------------------------------------------------------- fakes

var disk: [disk_sectors][bps]u8 = @splat(@splat(0));
var fat: [8]u32 = @splat(0);
var read_err: u16 = 0;
var write_err: u16 = 0;

fn reset() void {
    for (&disk) |*s| s.* = @splat(@splat(0));
    for (&fat) |*f| f.* = 0;
    read_err = 0;
    write_err = 0;
}

/// The fake FAT: `fat[cluster - 2]`, entry 0 is the first data cluster.
fn fatToSector(buf: [*c]u8) void {
    for (fat, 0..) |v, i| {
        const at = (i + 2) * 4;
        buf[at] = @truncate(v);
        buf[at + 1] = @truncate(v >> 8);
        buf[at + 2] = @truncate(v >> 16);
        buf[at + 3] = @truncate(v >> 24);
    }
}

fn fatFromSector(buf: [*c]u8) void {
    for (0..fat.len) |i| {
        const at = (i + 2) * 4;
        fat[i] = @as(u32, buf[at]) | (@as(u32, buf[at + 1]) << 8) |
            (@as(u32, buf[at + 2]) << 16) | (@as(u32, buf[at + 3]) << 24);
    }
}

fn read_block_backend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (read_err != 0) return read_err;
    if (lba >= fat_lba and lba < fat_lba + fat_secs) {
        fatToSector(buf);
        return ok;
    }
    const i: usize = @intCast(lba - data_lba);
    @memcpy(buf[0..bps], &disk[i]);
    return ok;
}

fn write_block_backend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (write_err != 0) return write_err;
    if (lba >= fat_lba and lba < fat_lba + fat_secs) {
        fatFromSector(@constCast(buf));
        return ok;
    }
    const i: usize = @intCast(lba - data_lba);
    @memcpy(&disk[i], buf[0..bps]);
    return ok;
}

var sec_buf: [c.k_ra8_fs_sector_max]u8 = @splat(0);

export fn priv_sec_io() callconv(.c) [*c]u8 {
    return &sec_buf;
}

export fn priv_exfat_find_bitmap(m: [*c]const c.ra8_fs_mount_t, out_clus: [*c]u32, out_len: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_clus.* = 4; // cluster_to_lba(4) = 1000 + 2 = the bitmap window
    out_len.* = 1024;
    return ok;
}

export fn priv_alloc_file_slot() callconv(.c) ?*c.ra8_fs_file_t {
    return null;
}

export fn priv_parse_bpb_into_mount(m: [*c]c.ra8_fs_mount_t) callconv(.c) u16 {
    _ = m;
    return 0;
}

export fn priv_exfat_lookup(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out_strm: [*c]u8, out_attr: [*c]u8) callconv(.c) u16 {
    _ = .{ m, path, out_strm, out_attr };
    return ok;
}

export fn priv_exfat_free_clusters(m: [*c]const c.ra8_fs_mount_t, strm: [*c]const u8) callconv(.c) u16 {
    _ = .{ m, strm };
    return ok;
}

export fn priv_exfat_flush_set(f: [*c]c.ra8_fs_file_t) callconv(.c) u16 {
    _ = f;
    return ok;
}

export fn priv_exfat_resolve_parent(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out: [*c]c.exfat_dir_t, leaf: [*c][*c]const u8) callconv(.c) u16 {
    _ = m;
    _ = out;
    leaf.* = path;
    return ok;
}

export fn priv_alloc_eoc_cluster(m: [*c]const u8, out_c: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_c.* = 6;
    return 0;
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
    m.first_fat_lba = fat_lba;
    m.fat_size_sectors = fat_secs;
    m.num_fats = 1;
    m.count_of_clusters = 64;
    m.root_cluster = 2;
    al.priv_fat_cache_reset();
    return m;
}

fn bindAndLba(m: *c.ra8_fs_mount_t) u64 {
    al.priv_alloc_state_bind(m);
    var lba: u64 = 0;
    std.debug.assert(al.priv_exfat_bitmap_lba(m, &lba) == ok);
    return lba;
}

fn fatAt(cluster: u32) u32 {
    return fat[cluster - 2];
}

// ---------------------------------------------------------------- tests

test "grow_one seeds an empty allocation at the first free cluster" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    const lba = bindAndLba(&m);
    try std.testing.expectEqual(bmp_lba_want, lba);

    try std.testing.expectEqual(ok, st.priv_exfat_ensure_clusters(&f, 2));
    try std.testing.expectEqual(@as(u32, 2), f.alloc_clusters);
    try std.testing.expectEqual(@as(u32, 2), f.first_cluster);
    try std.testing.expectEqual(@as(u32, 2), f.cur_cluster);
    try std.testing.expectEqual(@as(u32, 3), f.tail_cluster);
    try std.testing.expectEqual(@as(u32, 2), f.walk_cache_cluster);
    try std.testing.expectEqual(@as(u32, 3), fatAt(2));
    try std.testing.expectEqual(eoc, fatAt(3));
    try std.testing.expectEqual(@as(u8, 0b11), disk[2][0]);
    try std.testing.expectEqual(@as(u32, 4), al.priv_alloc_hint_get(&m));
}

test "pick_cluster prefers a free preference and scans around a taken one" {
    reset();
    var m = mount();
    const lba = bindAndLba(&m);
    var out: u32 = 0;
    try std.testing.expectEqual(ok, st.internal_exfat_pick_cluster(&m, lba, 7, &out));
    try std.testing.expectEqual(@as(u32, 7), out);

    disk[2][0] |= 1 << 5; // cluster 7 busy
    al.priv_alloc_hint_set(&m, 4);
    try std.testing.expectEqual(ok, st.internal_exfat_pick_cluster(&m, lba, 7, &out));
    try std.testing.expectEqual(@as(u32, 4), out);
}

test "pick_cluster scans when the preference is outside the data region" {
    reset();
    var m = mount();
    const lba = bindAndLba(&m);
    var out: u32 = 0;
    try std.testing.expectEqual(ok, st.internal_exfat_pick_cluster(&m, lba, 1, &out));
    try std.testing.expectEqual(first_data, out);
    try std.testing.expectEqual(ok, st.internal_exfat_pick_cluster(&m, lba, first_data + 64, &out));
    try std.testing.expectEqual(first_data, out);
}

test "materialize_run chains the run and points the tail at next" {
    reset();
    var m = mount();
    try std.testing.expectEqual(ok, st.internal_exfat_materialize_run(&m, 3, 3, 5, 9));
    try std.testing.expectEqual(@as(u32, 4), fatAt(3));
    try std.testing.expectEqual(@as(u32, 5), fatAt(4));
    try std.testing.expectEqual(@as(u32, 9), fatAt(5));
}

test "link_cluster leaves an empty allocation untouched" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    try std.testing.expectEqual(ok, st.internal_exfat_link_cluster(&f, 5));
    try std.testing.expectEqual(@as(u32, 0), fat[0]);
    try std.testing.expectEqual(@as(u32, 0), fat[3]);
}

test "link_cluster keeps a NoFatChain run contiguous without FAT writes" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    f.no_fat_chain = 1;
    f.first_cluster = 5;
    f.alloc_clusters = 3;
    f.tail_cluster = 7;
    try std.testing.expectEqual(ok, st.internal_exfat_link_cluster(&f, 8));
    try std.testing.expectEqual(@as(u32, 1), f.no_fat_chain);
    for (fat) |v| try std.testing.expectEqual(@as(u32, 0), v);
}

test "link_cluster converts a broken NoFatChain run into a real chain" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    f.no_fat_chain = 1;
    f.first_cluster = 5;
    f.alloc_clusters = 3;
    f.tail_cluster = 7;
    disk[2][0] = 0b01111000; // clusters 5..8 busy, 9 free
    al.priv_alloc_state_bind(&m);
    al.priv_alloc_hint_set(&m, 9);

    try std.testing.expectEqual(ok, st.internal_exfat_grow_one(&f));
    try std.testing.expectEqual(@as(u32, 0), f.no_fat_chain);
    try std.testing.expectEqual(@as(u32, 4), f.alloc_clusters);
    try std.testing.expectEqual(@as(u32, 9), f.tail_cluster);
    try std.testing.expectEqual(@as(u32, 6), fatAt(5));
    try std.testing.expectEqual(@as(u32, 7), fatAt(6));
    try std.testing.expectEqual(@as(u32, 9), fatAt(7));
    try std.testing.expectEqual(eoc, fatAt(9));
}

test "grow_one extends a chained allocation from the tail" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    f.no_fat_chain = 0;
    f.first_cluster = 2;
    f.alloc_clusters = 1;
    f.tail_cluster = 2;
    disk[2][0] = 0b1; // cluster 2 busy
    al.priv_alloc_state_bind(&m);
    al.priv_alloc_hint_set(&m, 3);

    try std.testing.expectEqual(ok, st.internal_exfat_grow_one(&f));
    try std.testing.expectEqual(@as(u32, 2), f.alloc_clusters);
    try std.testing.expectEqual(@as(u32, 3), f.tail_cluster);
    try std.testing.expectEqual(@as(u32, 3), fatAt(2));
    try std.testing.expectEqual(eoc, fatAt(3));
}

test "ensure_clusters tops the allocation up to the ask" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    al.priv_alloc_state_bind(&m);

    try std.testing.expectEqual(ok, st.priv_exfat_ensure_clusters(&f, 3));
    try std.testing.expectEqual(@as(u32, 3), f.alloc_clusters);
    try std.testing.expectEqual(@as(u32, 4), f.tail_cluster);
    try std.testing.expectEqual(@as(u32, 3), fatAt(2));
    try std.testing.expectEqual(@as(u32, 4), fatAt(3));
    try std.testing.expectEqual(eoc, fatAt(4));
}

test "a backend write failure surfaces from grow_one before anything links" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    al.priv_alloc_state_bind(&m);
    write_err = 7;

    const e = st.priv_exfat_ensure_clusters(&f, 1);
    try std.testing.expect(e != ok);
    try std.testing.expectEqual(@as(u32, 0), f.alloc_clusters);
}
