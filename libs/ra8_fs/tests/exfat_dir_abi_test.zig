// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
// Host tests for the exFAT directory ABI (path resolution, mkdir, rmdir).
// The fakes stand in for every cross-module symbol the ABI reaches through
// fs_c.zig: a scripted directory-image tree resolves paths, a flat byte disk
// backs the sector primitives, and the counters record what mkdir and rmdir
// did on the way.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const dir_abi = fs.exfat_dir;

const ok: u16 = c.k_ra8_ok;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_not_found: u16 = c.k_ra8_err_not_found;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const err_io: u16 = c.k_ra8_err_not_supported; // k_ra8_err_io no longer exists post-port; knob-injection tests need rework regardless
const err_exists: u16 = c.k_ra8_err_exists;
const err_not_empty: u16 = c.k_ra8_err_not_empty;
const err_protocol: u16 = c.k_ra8_err_protocol_error;

const entry_bytes: usize = @intCast(c.k_exfat_entry_bytes);
const name_cap: u32 = @intCast(c.k_exfat_name_cap);
const set_max: u32 = @intCast(c.k_exfat_set_max_entries);
const name_per_entry: u32 = @intCast(c.k_exfat_name_per_entry);
const max_set_bytes: u32 = @intCast(c.k_exfat_max_set_bytes);
const sector_max: usize = @intCast(c.k_ra8_fs_sector_max);

const entry_file: u8 = @intCast(c.k_exfat_entry_file);
const entry_stream: u8 = @intCast(c.k_exfat_entry_stream);
const entry_eod: u8 = @intCast(c.k_exfat_entry_eod);
const inuse_bit: u8 = @intCast(c.k_exfat_inuse_bit);
const attr_directory: u8 = @intCast(c.k_exfat_attr_directory);

const off_file_attr: usize = @intCast(c.k_exfat_off_file_attr);
const off_file_secnt: usize = @intCast(c.k_exfat_off_file_secnt);
const off_file_csum: usize = @intCast(c.k_exfat_off_file_csum);
const strm_off_flags: usize = @intCast(c.k_exfat_strm_off_flags);
const strm_off_nlen: usize = @intCast(c.k_exfat_strm_off_nlen);
const off_strm_hash: usize = @intCast(c.k_exfat_off_strm_hash);
const off_strm_valid: usize = @intCast(c.k_exfat_off_strm_valid);
const strm_off_clus: usize = @intCast(c.k_exfat_strm_off_clus);
const strm_off_dlen: usize = @intCast(c.k_exfat_strm_off_dlen);
const name_off: usize = @intCast(c.k_exfat_name_off);
const first_data: u32 = @intCast(c.k_cluster_first_data);

const entry_name: u8 = 0xC1;
const entry_bitmap: u8 = @intCast(c.k_exfat_entry_bitmap);
const secflag_alloc: u8 = @intCast(c.k_exfat_secflag_alloc);
const err_not_supported: u16 = c.k_ra8_err_not_supported;
const bps: u32 = 512;
const spc: u32 = 1; // one sector per cluster: cluster N maps to one LBA
const root_cluster: u32 = 2;
const data_start_lba: u64 = 64;

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Scripted directory tree on a real disk backend
// ---------------------------------------------------------------------------

const fat_lba: u64 = 4;
const fat_secs: u64 = 1;

var disk: [128][sector_max]u8 = undefined;
var m0: c.ra8_fs_mount_t = undefined;

fn clusLba(clus: u32) usize {
    return @intCast(data_start_lba + (clus - first_data) * spc);
}

fn wr32(p: []u8, at: usize, v: u32) void {
    p[at] = @truncate(v);
    p[at + 1] = @truncate(v >> 8);
    p[at + 2] = @truncate(v >> 16);
    p[at + 3] = @truncate(v >> 24);
}

/// Lay one real directory-entry set (File + Stream + one Name) for `name`
/// at `slot` of `clus`. The rest of the disk reads as zero, which the walk
/// sees as EOD, so every directory is a single-cluster image.
fn laySet(clus: u32, slot: usize, name: []const u8, attr: u8, first: u32, live: bool) void {
    // Deletion is the File entry's in-use bit only: secondaries keep their
    // type codes (0xC0/0xC1 carry the secondary category, not an in-use bit).
    const base = slot * entry_bytes;
    const buf = &disk[clusLba(clus)];
    buf[base] = if (live) entry_file else (entry_file & ~inuse_bit);
    buf[base + 1] = 2; // SecondaryCount: stream + one Name entry
    buf[base + off_file_attr] = attr;
    const s = base + entry_bytes;
    buf[s] = entry_stream;
    buf[s + strm_off_flags] = @intCast(c.k_exfat_secflag_no_fat);
    buf[s + strm_off_nlen] = @intCast(name.len);
    wr32(buf[s..], strm_off_clus, first);
    wr32(buf[s..], strm_off_dlen, 0);
    const n = base + 2 * entry_bytes;
    buf[n] = entry_name;
    for (name, 0..) |ch, i| buf[n + name_off + i * 2] = ch;
    buf[n + name_off + name.len * 2] = 0xFF;
    buf[n + name_off + name.len * 2 + 1] = 0xFF;
}

var slot_n: [64]usize = @splat(0);

fn addSlot(cluster: u32, name: []const u8, attr: u8, first: u32, live: bool) void {
    laySet(cluster, slot_n[cluster], name, attr, first, live);
    slot_n[cluster] += 3;
}

/// Lay the $Bitmap metadata set at the root's next slot: one single-slot
/// primary entry whose FirstCluster/DataLength pair lives at the stream
/// offsets, with the bitmap image itself at cluster `bmp_clus`. Its first
/// two bits come up set: cluster 2 is the root, cluster 3 the bitmap.
fn layBitmapRoot(bmp_clus: u32, len: u32) void {
    const slot = slot_n[root_cluster];
    slot_n[root_cluster] += 1;
    const base = slot * entry_bytes;
    const buf = &disk[clusLba(root_cluster)];
    buf[base] = entry_bitmap | inuse_bit;
    wr32(buf[base..], strm_off_clus, bmp_clus);
    wr32(buf[base..], strm_off_dlen, len);
    disk[clusLba(bmp_clus)][0] = 0x03;
}

/// A directory set carries a nonzero DataLength: free_clusters sizes the
/// run from it, so rmdir tests lay directories with a real length.
fn layDir(clus: u32, slot: usize, name: []const u8, first: u32, live: bool) void {
    laySet(clus, slot, name, attr_directory, first, live);
    const buf = &disk[clusLba(clus)];
    wr32(buf[slot * entry_bytes + entry_bytes ..], strm_off_dlen, bps * spc);
}

fn testReadBackend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (lba >= fat_lba and lba < fat_lba + fat_secs) {
        // Zero FAT: no chains; walks end at each cluster's EOD.
        @memset(buf[0..sector_max], 0);
        return 0;
    }
    if (lba == fail_read_lba) return err_not_supported;
    if (lba >= disk.len) return err_not_found;
    @memcpy(buf[0..sector_max], &disk[@intCast(lba)]);
    return 0;
}

fn testWriteBackend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (lba == fail_write_lba) return err_not_supported;
    if (lba >= disk.len) return err_not_found;
    @memcpy(&disk[@intCast(lba)], buf[0..sector_max]);
    return 0;
}

fn mkMount() c.ra8_fs_mount_t {
    var m: c.ra8_fs_mount_t = std.mem.zeroes(c.ra8_fs_mount_t);
    m.count_of_clusters = 64;
    m.bytes_per_sector = bps;
    m.sectors_per_cluster = spc;
    m.root_cluster = root_cluster;
    m.first_data_lba = data_start_lba;
    m.first_fat_lba = fat_lba;
    m.fat_size_sectors = fat_secs;
    m.num_fats = 1;
    // The real IO path fail-closes on a null backend (not_supported), so
    // every mount carries the scripted one the stub-era seams replaced.
    m.backend.read_block = testReadBackend;
    m.backend.write_block = testWriteBackend;
    return m;
}

fn resetFs() void {
    disk = @splat(@splat(0));
    slot_n = @splat(0);
    m0 = mkMount();
    find_err = 0;
    find_set_err = 0;
    next_err = 0;
    space_err = 0;
    wdir_err = 0;
    wdir_count = 0;
    mark_count = 0;
    clear_count = 0;
    hint_count = 0;
    drop_count = 0;
    free_count = 0;
    scan_free = 9;
    space_clus = root_cluster;
    space_idx = 2;
    fail_read_lba = no_lba;
    fail_write_lba = no_lba;
}

fn stdImage(cluster: u32) void {
    _ = cluster; // a zeroed cluster already reads as EOD-only
}

// Recorded calls.
var mark_count: u32 = 0;
var mark_lba: u64 = 0;
var mark_clus: u32 = 0;
var clear_count: u32 = 0;
var clear_lba: u64 = 0;
var clear_clus: u32 = 0;
var hint_count: u32 = 0;
var hint_clus: u32 = 0;
var wdir_count: u32 = 0;
var wdir_cluster: [8]u32 = undefined;
var wdir_index: [8]u32 = undefined;
var wdir_clus_field: [8]u32 = undefined;
var wdir_entry0: [8]u8 = undefined;
var wdir_set: [8][max_set_bytes]u8 = undefined;
var wdir_bytes: [8]u32 = undefined;
var wdir_err: u16 = 0;
var drop_count: u32 = 0;
var drop_n: u32 = 0;
var free_count: u32 = 0;
var free_clus: u32 = 0;
var scan_free: u32 = 9;
var space_clus: u32 = 0;
var space_idx: u32 = 0;
var find_err: u16 = 0;
var find_set_err: u16 = 0;
var next_err: u16 = 0;
var space_err: u16 = 0;

// Scripted-backend failure injection: the given LBA's next read or write
// reports not_supported, everything else passes.
const no_lba: u64 = std.math.maxInt(u64);
var fail_read_lba: u64 = no_lba;
var fail_write_lba: u64 = no_lba;

fn rd32(p: []const u8, off: usize) u32 {
    return std.mem.readInt(u32, p[off..][0..4], .little);
}

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "resolve_parent walks components and hands back parent plus leaf" {
    resetFs();
    stdImage(5);
    stdImage(6);
    addSlot(root_cluster, "a", attr_directory, 5, true);
    addSlot(5, "b", attr_directory, 6, true);
    var parent: c.exfat_dir_t = undefined;
    var leaf: [*c]const u8 = null;
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_resolve_parent(&m0, "/a/b/leaf.txt", &parent, &leaf));
    try std.testing.expectEqual(@as(u32, 6), parent.cluster);
    try std.testing.expectEqualStrings("leaf.txt", std.mem.span(@as([*:0]const u8, @ptrCast(leaf))));
}

test "resolve_parent refuses a file as an intermediate component" {
    resetFs();
    addSlot(root_cluster, "f", 0x20, 5, true);
    var parent: c.exfat_dir_t = undefined;
    var leaf: [*c]const u8 = null;
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_resolve_parent(&m0, "/f/x", &parent, &leaf));
}

test "resolve_parent reports a missing component and a too-deep path" {
    resetFs();
    var parent: c.exfat_dir_t = undefined;
    var leaf: [*c]const u8 = null;
    try std.testing.expectEqual(err_not_found, dir_abi.priv_exfat_resolve_parent(&m0, "/nope/x", &parent, &leaf));
    // Too deep: one component past the path-depth cap. The chain is laid
    // out in full so the walk fails on depth, not on a missing entry.
    laySet(root_cluster, 0, "a", attr_directory, 5, true);
    var cl: u32 = 5;
    while (cl < 5 + 32) : (cl += 1) laySet(cl, 0, "a", attr_directory, cl + 1, true);
    const deep = "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a" ++ "/a";
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_resolve_parent(&m0, deep, &parent, &leaf));
}

test "resolve_dir answers the root, a nested dir, and a trailing slash" {
    resetFs();
    stdImage(5);
    addSlot(root_cluster, "a", attr_directory, 5, true);
    var out: c.exfat_dir_t = undefined;
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_resolve_dir(&m0, "/", &out));
    try std.testing.expectEqual(root_cluster, out.cluster);
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_resolve_dir(&m0, "/a", &out));
    try std.testing.expectEqual(@as(u32, 5), out.cluster);
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_resolve_dir(&m0, "/a/", &out));
    try std.testing.expectEqual(@as(u32, 5), out.cluster);
}

test "lookup resolves and finds, but the root is not an entry set" {
    resetFs();
    stdImage(5);
    addSlot(root_cluster, "a", attr_directory, 5, true);
    addSlot(5, "one", 0x20, 7, true);
    var strm: [entry_bytes]u8 = undefined;
    var attr: u8 = 0;
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_lookup(&m0, "/a/one", &strm, &attr));
    try std.testing.expectEqual(@as(u32, 7), rd32(&strm, strm_off_clus));
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_lookup(&m0, "/", &strm, &attr));
}

test "enter rejects a name over the unit cap through the no_mem path" {
    resetFs();
    const long_name: [65]u8 = @splat('x');
    var path_buf: [80]u8 = undefined;
    const path = blk: {
        const pl = std.fmt.bufPrint(&path_buf, "/{s}/y", .{long_name}) catch unreachable;
        path_buf[pl.len] = 0;
        break :blk path_buf[0..pl.len :0];
    };
    var parent: c.exfat_dir_t = undefined;
    var leaf: [*c]const u8 = null;
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_resolve_parent(&m0, path, &parent, &leaf));
}

test "zero_cluster zeroes every sector of the cluster" {
    resetFs();
    const lba = data_start_lba + @as(u64, 9 - first_data) * spc;
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_zero_cluster(&m0, 9));
    try std.testing.expectEqual(@as(u8, 0), disk[@intCast(lba)][0]);
    try std.testing.expectEqual(@as(u8, 0), disk[@intCast(lba + 1)][sector_max - 1]);
}

test "seal_cluster rewrites EOD markers as deleted File entries" {
    resetFs();
    const lba = data_start_lba + @as(u64, 9 - first_data) * spc;
    disk[@intCast(lba)][0] = entry_eod;
    disk[@intCast(lba)][32] = entry_eod;
    disk[@intCast(lba)][64] = entry_file | inuse_bit; // live entry: untouched
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_seal_cluster(&m0, 9));
    try std.testing.expectEqual(entry_file & ~inuse_bit, disk[@intCast(lba)][0]);
    try std.testing.expectEqual(entry_file & ~inuse_bit, disk[@intCast(lba)][32]);
    try std.testing.expectEqual(entry_file | inuse_bit, disk[@intCast(lba)][64]);
}

test "mkdir links a directory set with the cluster, allocation length and checksum" {
    resetFs();
    layBitmapRoot(3, 8);
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_mkdir(&m0, "/newdir"));
    // The parent's free run starts past the $Bitmap entry: slot 1, not 0.
    const root_lba = clusLba(root_cluster);
    try std.testing.expectEqual(entry_file | inuse_bit, disk[root_lba][1 * entry_bytes]);
    const s = 2 * entry_bytes;
    try std.testing.expectEqual(@as(u32, 4), rd32(disk[root_lba][s..], strm_off_clus));
    try std.testing.expectEqual(@as(u32, bps * spc), rd32(disk[root_lba][s..], off_strm_valid));
    try std.testing.expectEqual(@as(u32, bps * spc), rd32(disk[root_lba][s..], strm_off_dlen));
    // The allocator marked the new cluster: bits for clusters 2, 3 and 4.
    try std.testing.expectEqual(@as(u8, 0b0000_0111), disk[clusLba(3)][0]);
}

test "mkdir set bytes: attribute, secondary count, name units, checksum" {
    resetFs();
    layBitmapRoot(3, 8);
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_mkdir(&m0, "/sub"));
    const set = disk[clusLba(root_cluster)][1 * entry_bytes ..][0 .. 3 * entry_bytes];
    try std.testing.expectEqual(entry_file | inuse_bit, set[0]);
    try std.testing.expectEqual(@as(u8, 2), set[off_file_secnt]); // File + Stream + 1 Name
    try std.testing.expectEqual(@as(u16, attr_directory), std.mem.readInt(u16, set[off_file_attr..][0..2], .little));
    const strm = set[entry_bytes..];
    try std.testing.expectEqual(entry_stream, strm[0]);
    try std.testing.expectEqual(secflag_alloc, strm[strm_off_flags]); // alloc possible, no FAT chain
    try std.testing.expectEqual(@as(u8, 3), strm[strm_off_nlen]);
    try std.testing.expectEqual(@as(u16, 0x382E), std.mem.readInt(u16, strm[off_strm_hash..][0..2], .little));
    try std.testing.expectEqual(@as(u32, bps * spc), rd32(strm, off_strm_valid));
    try std.testing.expectEqual(@as(u32, bps * spc), rd32(strm, strm_off_dlen));
    const name_e = set[2 * entry_bytes ..];
    try std.testing.expectEqual(entry_name, name_e[0]);
    try std.testing.expectEqual(@as(u16, 's'), std.mem.readInt(u16, name_e[name_off..][0..2], .little));
    try std.testing.expectEqual(@as(u16, 'u'), std.mem.readInt(u16, name_e[name_off + 2 ..][0..2], .little));
    try std.testing.expectEqual(@as(u16, 'b'), std.mem.readInt(u16, name_e[name_off + 4 ..][0..2], .little));
    // The on-disk SetChecksum matches the on-disk set: zero the field and
    // re-derive it with the real checksum builder.
    var probe: [3 * entry_bytes]u8 = undefined;
    @memcpy(&probe, set);
    const expect_csum = std.mem.readInt(u16, set[off_file_csum..][0..2], .little);
    probe[off_file_csum] = 0;
    probe[off_file_csum + 1] = 0;
    try std.testing.expectEqual(expect_csum, fs.exfat_write.priv_exfat_set_checksum(&probe, probe.len));
}

test "mkdir refuses the root, an over-long name, and an existing name" {
    resetFs();
    addSlot(root_cluster, "dup", 0x20, 7, true);
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_mkdir(&m0, "/"));
    const long_name: [65]u8 = @splat('y');
    var path_buf: [80]u8 = undefined;
    const path = blk: {
        const pl = std.fmt.bufPrint(&path_buf, "/{s}", .{long_name}) catch unreachable;
        path_buf[pl.len] = 0;
        break :blk path_buf[0..pl.len :0];
    };
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_mkdir(&m0, path));
    try std.testing.expectEqual(err_exists, dir_abi.priv_exfat_mkdir(&m0, "/dup"));
}

test "mkdir rolls the allocated cluster back when the set write fails" {
    resetFs();
    layBitmapRoot(3, 8);
    fail_write_lba = clusLba(root_cluster); // the parent slot write is the one that fails
    try std.testing.expectEqual(err_not_supported, dir_abi.priv_exfat_mkdir(&m0, "/late"));
    // The cluster came back: the bitmap holds only the root and $Bitmap bits.
    try std.testing.expectEqual(@as(u8, 0b0000_0011), disk[clusLba(3)][0]);
    // And the parent holds nothing new.
    try std.testing.expectEqual(@as(u8, 0), disk[clusLba(root_cluster)][1 * entry_bytes]);
}

test "rmdir retires the set and frees the cluster of an empty directory" {
    resetFs();
    addSlot(root_cluster, "a", attr_directory, 5, true);
    layDir(5, 0, "empty", 6, true);
    layBitmapRoot(3, 8);
    disk[clusLba(3)][0] |= 0x10; // cluster 6 starts allocated
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_rmdir(&m0, "/a/empty"));
    // The victim's set is retired in place: the in-use bit goes off every entry.
    const victim_lba = clusLba(5);
    try std.testing.expectEqual(entry_file & ~inuse_bit, disk[victim_lba][0]);
    try std.testing.expectEqual(entry_stream & ~inuse_bit, disk[victim_lba][entry_bytes]);
    try std.testing.expectEqual(entry_name & ~inuse_bit, disk[victim_lba][2 * entry_bytes]);
    // The parent's own set still lives.
    try std.testing.expectEqual(entry_file | inuse_bit, disk[clusLba(root_cluster)][0]);
    // And cluster 6 is free again.
    try std.testing.expectEqual(@as(u8, 0b0000_0011), disk[clusLba(3)][0]);
}

test "rmdir refuses a directory that still holds a live entry" {
    resetFs();
    stdImage(5);
    stdImage(7);
    addSlot(root_cluster, "a", attr_directory, 5, true);
    addSlot(5, "full", attr_directory, 7, true);
    addSlot(7, "keep.txt", 0x20, 12, true);
    try std.testing.expectEqual(err_not_empty, dir_abi.priv_exfat_rmdir(&m0, "/a/full"));
    try std.testing.expectEqual(@as(u32, 0), drop_count);
    try std.testing.expectEqual(@as(u32, 0), free_count);
}

test "rmdir discounts deleted remnants and stops at the run's end" {
    resetFs();
    addSlot(root_cluster, "a", attr_directory, 5, true);
    layDir(5, 0, "tidy", 8, true);
    addSlot(8, "gone.txt", 0x20, 12, false); // a deleted remnant in the victim
    layBitmapRoot(3, 8);
    disk[clusLba(3)][0] |= 0x40; // cluster 8 starts allocated
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_rmdir(&m0, "/a/tidy"));
    try std.testing.expectEqual(entry_file & ~inuse_bit, disk[clusLba(5)][0]);
    try std.testing.expectEqual(@as(u8, 0b0000_0011), disk[clusLba(3)][0]);
}

test "rmdir refuses the root, a file, and propagates lookup failures" {
    resetFs();
    addSlot(root_cluster, "f.txt", 0x20, 12, true);
    layBitmapRoot(3, 8);
    disk[clusLba(3)][1] |= 0x04; // cluster 12 (bit index 10) starts allocated
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_rmdir(&m0, "/"));
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_rmdir(&m0, "/f.txt"));
    // A backend failure inside the lookup propagates: the root's sector won't read.
    fail_read_lba = clusLba(root_cluster);
    try std.testing.expectEqual(err_not_supported, dir_abi.priv_exfat_rmdir(&m0, "/gone"));
    fail_read_lba = no_lba;
    // Nothing was freed or dropped: the file's bit still set, the set still live.
    try std.testing.expectEqual(@as(u8, 0x04), disk[clusLba(3)][1]);
    try std.testing.expectEqual(@as(u8, 0b0000_0011), disk[clusLba(3)][0]);
    try std.testing.expectEqual(entry_file | inuse_bit, disk[clusLba(root_cluster)][0]);
}
