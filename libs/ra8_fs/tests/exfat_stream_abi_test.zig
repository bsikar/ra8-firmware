//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT growth (RA8FW-1118 cluster growth, RA8FW-1119 directory growth):
//! cluster pick, run materialization, NoFatChain linking, one-cluster
//! growth, the ensure loop, the directory side of the same machinery --
//! survey, link, entry-set rewrite, append and grow-dir -- and the
//! stream-write half (RA8FW-1120): cluster_at, slice_at, close_gap,
//! write_stream, patch_stream, gather_set and flush_set -- against a
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
    for (&disk) |*s| s.* = @splat(0);
    for (&fat) |*f| f.* = 0;
    read_err = 0;
    write_err = 0;
    al.priv_alloc_state_reset();
    // the real table stands in: fill every file slot, none stays free
    while (true) {
        const f = fs.fat_state.priv_alloc_file_slot();
        if (f == null) break;
        f.*.in_use = 1;
    }
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

var sec_buf: *[c.k_ra8_fs_sector_max]u8 = fs.fat_bytes.io_sector;

// --- directory-growth fakes (RA8FW-1119): zero, seal, the entry cursor, ---
// --- the set checksum and the set write-back, each mirroring the real ----
// --- behavior against the fake disk --------------------------------------

const err_not_found: u16 = @intCast(c.k_ra8_err_not_found);
const entry_eod: u8 = 0x00;
const entry_file: u8 = 0x85;
const inuse_bit: u8 = 0x80;

/// Stage a minimal File + Stream entry set at `cluster`/`idx` on the fake
/// disk, with an intentionally stale set checksum.
fn stageSet(cluster: u32, idx: u32, secnt: u8) void {
    const ci: usize = @intCast(cluster - first_data);
    disk[ci][idx * 32] = entry_file;
    disk[ci][idx * 32 + 1] = secnt;
    disk[ci][idx * 32 + 2] = 0x12;
    disk[ci][idx * 32 + 3] = 0x34;
    const so: usize = (idx + 1) * 32;
    disk[ci][so] = 0xC0;
    disk[ci][so + 1] = 0x01; // AllocationPossible, NoFatChain clear
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

/// The stub-era find_bitmap answered with a canned cluster; the real one
/// walks the root dir for the $Bitmap primary entry. Lay it: first cluster 4
/// (lba 1002, the bitmap window), 8 bytes for the 64-cluster volume.
fn layBitmap() void {
    disk[0][0] = @intCast(c.k_exfat_entry_bitmap);
    std.mem.writeInt(u32, disk[0][@intCast(c.k_exfat_strm_off_clus)..][0..4], 4, .little);
    std.mem.writeInt(u32, disk[0][@intCast(c.k_exfat_strm_off_dlen)..][0..4], 8, .little);
}

fn bindAndLba(m: *c.ra8_fs_mount_t) u64 {
    layBitmap();
    al.priv_alloc_state_bind(m);
    var lba: u64 = 0;
    std.debug.assert(al.priv_exfat_bitmap_lba(m, &lba) == ok);
    return lba;
}

fn fatAt(cluster: u32) u32 {
    return fat[cluster - 2];
}

fn setChecksum(set: []const u8) u16 {
    var cs: u16 = 0;
    for (set, 0..) |b, i| {
        if (i == 2 or i == 3) continue;
        const hi: u16 = if ((cs & 1) != 0) 0x8000 else 0;
        cs = hi +% (cs >> 1) +% @as(u16, b);
    }
    return cs;
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
    _ = bindAndLba(&m);
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
    _ = bindAndLba(&m);
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
    _ = bindAndLba(&m);

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

// ------------------------------------------------------- directory growth

test "dir_survey reads a contiguous run arithmetically" {
    reset();
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 5;
    dir.contig_end = 8;
    var alloc: u32 = 0;
    var tail: u32 = 0;
    var nofat: u8 = 0;
    try std.testing.expectEqual(ok, st.internal_exfat_dir_survey(null, &dir, &alloc, &tail, &nofat));
    try std.testing.expectEqual(@as(u32, 3), alloc);
    try std.testing.expectEqual(@as(u32, 7), tail);
    try std.testing.expectEqual(@as(u8, 1), nofat);
}

test "dir_survey walks a FAT chain to its tail" {
    reset();
    var m = mount();
    fat[3] = 6;
    fat[4] = 7;
    fat[5] = eoc;
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 5;
    var alloc: u32 = 0;
    var tail: u32 = 0;
    var nofat: u8 = 0xFF;
    try std.testing.expectEqual(ok, st.internal_exfat_dir_survey(&m, &dir, &alloc, &tail, &nofat));
    try std.testing.expectEqual(@as(u32, 3), alloc);
    try std.testing.expectEqual(@as(u32, 7), tail);
    try std.testing.expectEqual(@as(u8, 0), nofat);
}

test "dir_survey faults when a chained run points below the heap" {
    reset();
    var m = mount();
    fat[3] = 0; // free entry: invalid successor inside a chain
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 5;
    var alloc: u32 = 0;
    var tail: u32 = 0;
    var nofat: u8 = 0;
    const e = st.internal_exfat_dir_survey(&m, &dir, &alloc, &tail, &nofat);
    try std.testing.expect(e != ok);
}

test "dir_link keeps a contiguous run contiguous without FAT writes" {
    reset();
    var nofat: u8 = 1;
    try std.testing.expectEqual(ok, st.internal_exfat_dir_link(null, 5, 3, 7, &nofat, 8));
    try std.testing.expectEqual(@as(u8, 1), nofat);
    for (fat) |v| try std.testing.expectEqual(@as(u32, 0), v);
}

test "dir_link converts a broken contiguous run into a real chain" {
    reset();
    var m = mount();
    var nofat: u8 = 1;
    try std.testing.expectEqual(ok, st.internal_exfat_dir_link(&m, 5, 3, 7, &nofat, 9));
    try std.testing.expectEqual(@as(u8, 0), nofat);
    try std.testing.expectEqual(@as(u32, 6), fatAt(5));
    try std.testing.expectEqual(@as(u32, 7), fatAt(6));
    try std.testing.expectEqual(@as(u32, 9), fatAt(7));
    try std.testing.expectEqual(eoc, fatAt(9));
}

test "dir_link links a chained run's tail" {
    reset();
    var m = mount();
    var nofat: u8 = 0;
    try std.testing.expectEqual(ok, st.internal_exfat_dir_link(&m, 5, 3, 7, &nofat, 9));
    try std.testing.expectEqual(@as(u8, 0), nofat);
    try std.testing.expectEqual(@as(u32, 9), fatAt(7));
    try std.testing.expectEqual(eoc, fatAt(9));
}

test "dir_relen patches the stream entry and rewrites the set" {
    reset();
    var m = mount();
    stageSet(7, 0, 1); // File + Stream at cluster 7, entry 0
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 5;
    dir.contig_end = 6;
    dir.self_cluster = 7;
    dir.self_index = 0;

    try std.testing.expectEqual(ok, st.internal_exfat_dir_relen(&m, &dir, 1024, 1));
    // the real set write-back lands on the volume: cluster 7's sector
    try std.testing.expectEqual(@as(u8, 0x03), disk[5][32 + @as(usize, @intCast(c.k_exfat_strm_off_flags))]); // alloc | nofat
    try std.testing.expectEqual(@as(u64, 1024), rd64(disk[5][32..], @intCast(c.k_exfat_strm_off_dlen)));
    try std.testing.expectEqual(@as(u64, 1024), rd64(disk[5][32..], @intCast(c.k_exfat_off_strm_valid)));
    try std.testing.expectEqual(setChecksum(disk[5][0..64]), rd16(disk[5][0..], @intCast(c.k_exfat_off_file_csum)));
}

test "dir_relen refuses a set beyond the writable cap" {
    reset();
    var m = mount();
    stageSet(7, 0, 7); // 1 + 7 = 8 entries, one past the writable cap
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.self_cluster = 7;
    dir.self_index = 0;
    const e = st.internal_exfat_dir_relen(&m, &dir, 1024, 1);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_err_not_supported)), e);
}

test "dir_append zeroes the new cluster before linking and seals the old tail" {
    reset();
    var m = mount();
    _ = bindAndLba(&m);
    disk[2][0] = 0x08; // cluster 5 used, 6.. free
    disk[6][10] = 0xAB; // garbage in the cluster about to be picked
    disk[5][0] = 0x00; // trailing EOD marker in the old tail
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 5;
    var nofat: u8 = 1;

    try std.testing.expectEqual(ok, st.internal_exfat_dir_append(&m, &dir, 3, 7, &nofat));
    try std.testing.expectEqual(@as(u8, 0), disk[6][10]); // zeroed before linking
    try std.testing.expectEqual(@as(u8, 0x48), disk[2][0]); // 5 and 8 marked used
    try std.testing.expectEqual(@as(u32, 9), al.priv_alloc_hint_get(&m));
    for (fat) |v| try std.testing.expectEqual(@as(u32, 0), v); // stayed contiguous
    try std.testing.expectEqual(@as(u8, 0x05), disk[5][0]); // EOD retired
}

test "grow_dir grows a subdirectory and rewrites its own entry set" {
    reset();
    var m = mount();
    _ = bindAndLba(&m);
    disk[2][0] = 0x08; // cluster 5 used, 6.. free
    disk[4][3] = 0xAB; // garbage in the cluster about to be picked
    stageSet(7, 0, 1); // the subdirectory's own set, in its parent
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 5;
    dir.contig_end = 6;
    dir.self_cluster = 7;
    dir.self_index = 0;

    try std.testing.expectEqual(ok, st.priv_exfat_grow_dir(&m, &dir));
    try std.testing.expectEqual(@as(u8, 0), disk[4][3]); // new cluster zeroed
    try std.testing.expectEqual(@as(u8, 0x18), disk[2][0]); // 5 and 6 marked used
    try std.testing.expectEqual(@as(u8, 0x05), disk[3][0]); // old tail sealed
    try std.testing.expectEqual(setChecksum(disk[5][0..64]), rd16(disk[5][0..], @intCast(c.k_exfat_off_file_csum))); // set rewritten
    try std.testing.expectEqual(@as(u8, 0x03), disk[5][33]); // NoFatChain flag back on
    try std.testing.expectEqual(@as(u64, 1024), std.mem.readInt(u64, disk[5][40..48], .little));
    try std.testing.expectEqual(@as(u32, 7), dir.contig_end);
}

test "grow_dir leaves the root's entry set alone" {
    reset();
    var m = mount();
    _ = bindAndLba(&m);
    disk[2][0] = 0x08; // cluster 5 used, 6.. free
    stageSet(7, 0, 1); // a subdirectory's set sits elsewhere on the volume
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 5;
    dir.contig_end = 6; // the root carries no self_cluster

    try std.testing.expectEqual(ok, st.priv_exfat_grow_dir(&m, &dir));
    try std.testing.expectEqual(@as(u16, 0x3412), rd16(disk[5][0..], 2)); // that set untouched
    try std.testing.expectEqual(@as(u8, 0x01), disk[5][33]);
    try std.testing.expectEqual(@as(u8, 0x18), disk[2][0]);
    try std.testing.expectEqual(@as(u32, 7), dir.contig_end);
}

test "grow_dir extends a chained directory over the FAT" {
    reset();
    var m = mount();
    _ = bindAndLba(&m);
    disk[2][0] = 0x08; // cluster 5 used, 6.. free
    fat[3] = eoc; // a chained one-cluster directory
    stageSet(7, 0, 1);
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 5;
    dir.self_cluster = 7;
    dir.self_index = 0;

    try std.testing.expectEqual(ok, st.priv_exfat_grow_dir(&m, &dir));
    try std.testing.expectEqual(@as(u32, 6), fatAt(5)); // tail linked
    try std.testing.expectEqual(eoc, fatAt(6)); // new tail capped
    try std.testing.expectEqual(@as(u8, 0x01), disk[5][33]); // NoFatChain stays clear
    try std.testing.expectEqual(@as(u64, 1024), std.mem.readInt(u64, disk[5][40..48], .little));
    try std.testing.expectEqual(@as(u32, 0), dir.contig_end);
}

// ------------------------------------------------- stream write (RA8FW-1120)

fn rd16(buf: []const u8, off: usize) u16 {
    return @as(u16, buf[off]) | (@as(u16, buf[off + 1]) << 8);
}

fn rd32(buf: []const u8, off: usize) u32 {
    return @as(u32, buf[off]) | (@as(u32, buf[off + 1]) << 8) |
        (@as(u32, buf[off + 2]) << 16) | (@as(u32, buf[off + 3]) << 24);
}

fn rd64(buf: []const u8, off: usize) u64 {
    return @as(u64, rd32(buf, off)) | (@as(u64, rd32(buf, off + 4)) << 32);
}

test "cluster_at is arithmetic on a contiguous file" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    f.no_fat_chain = 1;
    f.first_cluster = 5;
    var out: u32 = 0;
    try std.testing.expectEqual(ok, st.internal_exfat_cluster_at(&f, 2, &out));
    try std.testing.expectEqual(@as(u32, 7), out);
}

test "cluster_at walks a chain from the waypoint and faults past its end" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    f.first_cluster = 2;
    fat[0] = 3; // cluster 2 -> 3
    fat[1] = eoc; // cluster 3 -> EOC
    var out: u32 = 0;
    try std.testing.expectEqual(ok, st.internal_exfat_cluster_at(&f, 1, &out));
    try std.testing.expectEqual(@as(u32, 3), out);
    try std.testing.expectEqual(@as(u32, 1), f.walk_cache_idx);
    try std.testing.expectEqual(@as(u32, 3), f.walk_cache_cluster);
    try std.testing.expectEqual(c.k_ra8_err_invalid_state, st.internal_exfat_cluster_at(&f, 2, &out));
    // A backward seek restarts from the chain head, not the waypoint.
    try std.testing.expectEqual(ok, st.internal_exfat_cluster_at(&f, 0, &out));
    try std.testing.expectEqual(@as(u32, 2), out);
}

test "slice_at grows the allocation and lands the sector" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    _ = bindAndLba(&m);
    var lba: u64 = 0;
    var off: u32 = 0;
    var room: u32 = 0;
    try std.testing.expectEqual(ok, st.internal_exfat_slice_at(&f, 600, &lba, &off, &room));
    try std.testing.expectEqual(@as(u32, 2), f.alloc_clusters);
    try std.testing.expectEqual(data_lba + 1, lba);
    try std.testing.expectEqual(@as(u32, 88), off);
    try std.testing.expectEqual(@as(u32, bps - 88), room);
    try std.testing.expectEqual(@as(u32, 3), f.cur_cluster);
}

test "close_gap zero-fills the unwritten prefix" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    f.no_fat_chain = 1;
    f.first_cluster = 2;
    f.alloc_clusters = 1;
    f.valid_bytes = 0;
    f.offset = 5;
    f.size_bytes = 5;
    @memset(disk[0][0..10], 0xAA);
    try std.testing.expectEqual(ok, st.internal_exfat_close_gap(&f));
    for (disk[0][0..5]) |b| try std.testing.expectEqual(@as(u8, 0), b);
    try std.testing.expectEqual(@as(u8, 0xAA), disk[0][5]);
    try std.testing.expectEqual(@as(u64, 5), f.valid_bytes);
}

test "write_stream fills the gap then appends across a cluster boundary" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    _ = bindAndLba(&m);
    disk[2][0] = 0x01; // the root's own cluster is taken: the file lands off it
    f.valid_bytes = 0;
    f.offset = 3; // positioned past the prefix: a hole to fill first
    f.size_bytes = 3;
    var buf = "hello".*;
    try std.testing.expectEqual(ok, st.priv_exfat_write_stream(&f, &buf, 5));
    try std.testing.expectEqual(@as(u8, 0), disk[1][0]);
    try std.testing.expectEqual(@as(u8, 0), disk[1][2]);
    try std.testing.expectEqual(@as(u8, 'h'), disk[1][3]);
    try std.testing.expectEqual(@as(u64, 8), f.valid_bytes);
    try std.testing.expectEqual(@as(u64, 8), f.offset);
    try std.testing.expectEqual(@as(u64, 8), f.size_bytes);

    var big: [600]u8 = @splat(0xB7);
    try std.testing.expectEqual(ok, st.priv_exfat_write_stream(&f, &big, 600));
    try std.testing.expectEqual(@as(u32, 2), f.alloc_clusters);
    try std.testing.expectEqual(@as(u64, 608), f.size_bytes);
}

test "flush_set stamps, patches, archives and re-checksums the set" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    stageSet(2, 0, 2);
    f.entry_set_cluster = 2;
    f.entry_set_index = 0;
    f.entry_set_count = 2;
    f.alloc_clusters = 1;
    f.first_cluster = 2;
    f.no_fat_chain = 1;
    f.valid_bytes = 5;
    f.size_bytes = 5;
    try std.testing.expectEqual(ok, st.priv_exfat_flush_set(&f));
    // File entry: the archive bit is set and the checksum covers every byte
    // but its own two.
    try std.testing.expectEqual(@as(u8, 0x20), disk[0][@intCast(c.k_exfat_off_file_attr)] & 0x20);
    const cs = setChecksum(disk[0][0..64]);
    try std.testing.expectEqual(cs, rd16(disk[0][0..], @intCast(c.k_exfat_off_file_csum)));
    // Stream entry: allocated and contiguous, FirstCluster 2, lengths 5.
    try std.testing.expectEqual(@as(u8, 0x03), disk[0][32 + @as(usize, @intCast(c.k_exfat_strm_off_flags))]);
    const so: usize = 32;
    try std.testing.expectEqual(@as(u32, 2), rd32(disk[0][so..], @intCast(c.k_exfat_strm_off_clus)));
    try std.testing.expectEqual(@as(u64, 5), rd64(disk[0][so..], @intCast(c.k_exfat_off_strm_valid)));
    try std.testing.expectEqual(@as(u64, 5), rd64(disk[0][so..], @intCast(c.k_exfat_strm_off_dlen)));
}

test "flush_set keeps an empty file's stream entry cluster-free" {
    reset();
    var m = mount();
    var f = std.mem.zeroes(c.ra8_fs_file_t);
    f.mount = &m;
    stageSet(2, 0, 2);
    f.entry_set_cluster = 2;
    f.entry_set_index = 0;
    f.entry_set_count = 2;
    try std.testing.expectEqual(ok, st.priv_exfat_flush_set(&f));
    try std.testing.expectEqual(@as(u8, 0x01), disk[0][32 + @as(usize, @intCast(c.k_exfat_strm_off_flags))]);
    try std.testing.expectEqual(@as(u32, 0), rd32(disk[0][32..], @intCast(c.k_exfat_strm_off_clus)));
}
