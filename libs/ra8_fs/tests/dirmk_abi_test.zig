//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT read side (RA8FW-1087): volume parse, the up-case walk, the
//! directory cursor machinery, flat name matching, and the read open,
//! against this file's own fakes.

const std = @import("std");
const al = @import("ra8_fs").alloc;
const fs = @import("ra8_fs");

var lookup_any_rc: u16 = 0;
var lookup_lba: u64 = 0;
var lookup_off: u32 = 0;
var lookup_entry: [32]u8 = @splat(0);
var reserve_rc: u16 = 0;
var commit_rc: u16 = 0;
var commit_lba: u64 = 0;
var commit_off: u32 = 0;
var commit_entry: [32]u8 = @splat(0);
var stamp_calls: usize = 0;

// stand-in for the one helper the open/close port added (RA8FW-1092); the
// dirmk path never closes a file handle, so it only needs to exist
export fn priv_fat_entry_stamp_write(entry: [*c]u8) callconv(.c) void {
    _ = entry;
}
const ex = fs.exfat_read;
const upcase = fs.upcase;
const c = fs.c;

const bps: u32 = 512;
const spc: u32 = 1;
const cluster_bytes: u32 = bps * spc;
const per_cluster: u32 = cluster_bytes / 32;
const data_lba: u64 = 0;
const eoc: u32 = 0x0FFFFFF8;
const disk_sectors: usize = 256;

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
var resolve_err: u16 = 0;

fn reset() void {
    for (&disk) |*s| s.* = @splat(0);
    for (&fat) |*f| f.* = 0;
    read_err = 0;
    fat_read_err = 0;
    pool_used = 0;
    bpb_result = 0xAAAA;
    lookup_err = 0;
    lookup_strm = @splat(0);
    lookup_attr = 0;
    resolve_err = 0;
    @memset(&g_fs_scratch, 0);
}
var sec_buf: [c.k_ra8_fs_sector_max]u8 = @splat(0);

export fn priv_sec_io() callconv(.c) [*c]u8 {
    return &sec_buf;
}

/// The fake FAT: a small array served as raw 4-byte-entry sectors, indexed
/// `fat[cluster - 2]` so entry 0 is the first data cluster, matching how the
/// tests seed chains. The two reserved leading entries pad to zero.
pub const fat_lba: u64 = 900;
pub const fat_secs: u64 = 2;

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
        if (fat_read_err != 0) return fat_read_err;
        fatToSector(buf);
        return ok;
    }
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

export fn priv_exfat_link(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, name: [*c]const u16, nlen: u32, head: [*c]c.exfat_setpos_t, count: [*c]u32) callconv(.c) u16 {
    _ = m;
    _ = dir;
    _ = name;
    _ = nlen;
    _ = head;
    _ = count;
    return ok;
}

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
    m.first_fat_lba = fat_lba;
    m.fat_size_sectors = fat_secs;
    m.num_fats = 1;
    al.priv_fat_cache_reset();
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

// ---------------------------------------------------------------- tests

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

export fn priv_exfat_find_bitmap(m: [*c]const c.ra8_fs_mount_t, out_clus: [*c]u32, out_len: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_clus.* = 3;
    out_len.* = 1024;
    return 0;
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

export fn priv_exfat_set_checksum(set: [*c]const u8, bytes: u32) callconv(.c) u16 {
    _ = set;
    _ = bytes;
    return 0;
}

export fn priv_exfat_write_dir_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32, idx: u32, set: [*c]const u8, bytes: u32) callconv(.c) u16 {
    _ = .{ m, cluster, idx, set, bytes };
    return 0;
}

export fn priv_fat_entry_set_times(entry: [*c]u8, create: [*c]const c.ra8_fs_datetime_t, modify: [*c]const c.ra8_fs_datetime_t, access: [*c]const c.ra8_fs_datetime_t) callconv(.c) void {
    _ = .{ entry, create, modify, access };
}
fn priv_path_to_83(path: [*c]const u8, out11: [*c]u8) callconv(.c) u8 {
    _ = path;
    _ = out11;
    return 0;
}

export fn priv_sec_walk() callconv(.c) [*c]u8 {
    return &walk_sector;
}

fn write_block_backend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (write_fail_lba == lba) return err_io;
    if (lba >= fat_lba and lba < fat_lba + fat_secs) {
        fatFromSector(@constCast(buf));
        fat_writes += 1;
        return ok;
    }
    const i: usize = @intCast(lba - data_lba);
    @memcpy(&disk[i], buf[0..bps]);
    write_calls += 1;
    return 0;
}

// ------------------------------------------------- dirmk fakes + stubs

var err_io_val: u16 = 0;

const err_io: u16 = c.k_ra8_err_access_denied;

var erase_calls: usize = 0;
var erase_rc: u16 = 0;
var erase_name0: u8 = 0;
var alloc_rc: u16 = 0;
var alloc_calls: usize = 0;
var next_cluster: u32 = 0;
var fat_set_calls: usize = 0;
var last_fat_clus: u32 = 0;
var last_fat_val: u32 = 0;
var fat_writes: usize = 0;
var write_fail_lba: u64 = 0xFFFF_FFFF_FFFF_FFFF;
var write_calls: usize = 0;
var exfat_calls: usize = 0;
var exfat_rc: u16 = 0;

fn resetDirmk() void {
    lookup_any_rc = 0;
    lookup_lba = 0;
    lookup_off = 0;
    lookup_entry = @splat(0);
    reserve_rc = 0;
    commit_rc = 0;
    commit_lba = 0;
    commit_off = 0;
    commit_entry = @splat(0);
    erase_calls = 0;
    erase_rc = 0;
    erase_name0 = 0;
    alloc_rc = 0;
    alloc_calls = 0;
    next_cluster = 0;
    stamp_calls = 0;
    fat_writes = 0;
    write_fail_lba = 0xFFFF_FFFF_FFFF_FFFF;
    write_calls = 0;
    exfat_calls = 0;
    exfat_rc = 0;
    read_err = 0;
}

export fn priv_dir_erase_chain(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, lba: u64, off: u32, name: [*c]const u8) callconv(.c) u16 {
    _ = m;
    _ = loc;
    _ = lba;
    _ = off;
    erase_calls += 1;
    erase_name0 = name[0];
    return erase_rc;
}

export fn priv_alloc_eoc_cluster(m: [*c]const u8, out_c: [*c]u32) callconv(.c) u16 {
    _ = m;
    alloc_calls += 1;
    if (alloc_rc != ok) return alloc_rc;
    out_c.* = next_cluster;
    next_cluster += 1;
    return ok;
}

export fn priv_exfat_mkdir(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8) callconv(.c) u16 {
    _ = m;
    _ = path;
    exfat_calls += 1;
    return exfat_rc;
}

export fn priv_exfat_rmdir(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8) callconv(.c) u16 {
    _ = m;
    _ = path;
    exfat_calls += 1;
    return exfat_rc;
}

// --------------------------------------------------------------- tests

const err_exists: u16 = c.k_ra8_err_exists;
const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
const err_protocol: u16 = c.k_ra8_err_protocol_error;
const err_not_empty: u16 = c.k_ra8_err_not_empty;
const attr_dir: u8 = @intCast(c.k_ra8_fs_attr_directory);
const attr_lfn_v: u8 = @intCast(c.k_ra8_fs_attr_lfn);
const type_fat32 = c.k_ra8_fs_type_fat32;
const type_exfat = c.k_ra8_fs_type_exfat;

fn fatmount() c.ra8_fs_mount_t {
    var m = std.mem.zeroes(c.ra8_fs_mount_t);
    m.type = type_fat32;
    m.in_use = 1;
    m.bytes_per_sector = bps;
    m.backend.read_block = read_block_backend;
    m.backend.write_block = write_block_backend;
    m.sectors_per_cluster = spc;
    m.first_data_lba = data_lba;
    m.count_of_clusters = 64;
    // The real FAT entry layer reads through the mount geometry; the stubs
    // it replaces never needed these fields.
    m.first_fat_lba = fat_lba;
    m.fat_size_sectors = fat_secs;
    m.num_fats = 1;
    al.priv_fat_cache_reset();
    return m;
}

fn dotAt(sector: []u8, base: usize, dots: usize) void {
    for (0..32) |i| sector[base + i] = 0;
    for (0..11) |i| sector[base + i] = ' ';
    for (0..dots) |i| sector[base + i] = '.';
    sector[base + 11] = attr_dir;
}

test "mkdir lays the dot entries and links the parent entry" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = err_not_found;
    next_cluster = 5;
    commit_lba = 900;
    commit_off = 64;
    try std.testing.expectEqual(ok, fs.dirmk.ra8_fs_mkdir(&m, "NOTES"));
    // cluster 5 -> lba 3 with first_data_lba 0 and one sector per cluster
    try std.testing.expectEqual(@as(u8, '.'), disk[3][0]);
    for (1..11) |i| try std.testing.expectEqual(@as(u8, ' '), disk[3][i]);
    try std.testing.expectEqual(attr_dir, disk[3][11]);
    try std.testing.expectEqual(@as(u8, 5), disk[3][26]);
    try std.testing.expectEqual(@as(u8, 0), disk[3][27]);
    for (28..32) |i| try std.testing.expectEqual(@as(u8, 0), disk[3][i]);
    try std.testing.expectEqual(@as(u8, '.'), disk[3][32]);
    try std.testing.expectEqual(@as(u8, '.'), disk[3][33]);
    for (34..43) |i| try std.testing.expectEqual(@as(u8, ' '), disk[3][i]);
    try std.testing.expectEqual(attr_dir, disk[3][43]);
    try std.testing.expectEqual(@as(u8, 0), disk[3][58]); // parent is root: 0
    try std.testing.expectEqual(@as(u8, 0), disk[3][59]);
    for (64..512) |i| try std.testing.expectEqual(@as(u8, 0), disk[3][i]);
    try std.testing.expectEqual(@as(usize, 3), stamp_calls);
    try std.testing.expectEqual(@as(usize, 1), write_calls);
    try std.testing.expectEqual(@as(usize, 1), alloc_calls);
    try std.testing.expectEqual(@as(usize, 0), fat_writes);
    try std.testing.expectEqual(attr_dir, commit_entry[11]);
    try std.testing.expectEqual(@as(u8, 5), commit_entry[26]);
    try std.testing.expectEqual(@as(u32, 5), c.priv_entry_first_cluster(&commit_entry));
}

test "mkdir refuses an existing name before touching anything" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = ok;
    try std.testing.expectEqual(err_exists, fs.dirmk.ra8_fs_mkdir(&m, "NOTES"));
    try std.testing.expectEqual(@as(usize, 0), alloc_calls);
    try std.testing.expectEqual(@as(usize, 0), stamp_calls);
    try std.testing.expectEqual(@as(usize, 0), write_calls);
}

test "mkdir propagates a full directory and allocates nothing" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = err_not_found;
    reserve_rc = err_no_mem;
    try std.testing.expectEqual(err_no_mem, fs.dirmk.ra8_fs_mkdir(&m, "NOTES"));
    try std.testing.expectEqual(@as(usize, 0), alloc_calls);
}

test "mkdir propagates an allocation failure" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = err_not_found;
    alloc_rc = err_no_mem;
    try std.testing.expectEqual(err_no_mem, fs.dirmk.ra8_fs_mkdir(&m, "NOTES"));
    try std.testing.expectEqual(@as(usize, 0), stamp_calls);
    try std.testing.expectEqual(@as(usize, 0), write_calls);
}

test "a cluster-init write failure frees the new cluster" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = err_not_found;
    next_cluster = 5;
    fat[3] = eoc; // real free_chain: cluster 5 ends an EOC chain
    write_fail_lba = 3;
    const rc = fs.dirmk.ra8_fs_mkdir(&m, "NOTES");
    try std.testing.expect(err_io == rc);
    try std.testing.expectEqual(@as(u32, 0), fat[3]); // cluster 5 freed
}

test "a commit failure frees the new cluster" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = err_not_found;
    next_cluster = 5;
    fat[3] = eoc;
    commit_rc = err_io;
    try std.testing.expectEqual(err_io, fs.dirmk.ra8_fs_mkdir(&m, "NOTES"));
    try std.testing.expectEqual(@as(u32, 0), fat[3]); // cluster 5 freed
}

test "mkdir dispatch: null args, idle mount, exFAT routing" {
    reset();
    resetDirmk();
    var m = fatmount();
    try std.testing.expectEqual(err_null_ptr, fs.dirmk.ra8_fs_mkdir(null, "NOTES"));
    try std.testing.expectEqual(err_null_ptr, fs.dirmk.ra8_fs_mkdir(&m, null));
    m.in_use = 0;
    try std.testing.expectEqual(err_invalid_state, fs.dirmk.ra8_fs_mkdir(&m, "NOTES"));
    m.in_use = 1;
    m.type = type_exfat;
    exfat_rc = err_not_supported;
    try std.testing.expectEqual(err_not_supported, fs.dirmk.ra8_fs_mkdir(&m, "NOTES"));
    try std.testing.expectEqual(@as(usize, 1), exfat_calls);
}

test "rmdir removes an empty directory: chain freed, entry erased" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = ok;
    lookup_entry[0] = 'D';
    lookup_entry[11] = attr_dir;
    lookup_entry[26] = 3; // first cluster 3
    dotAt(&disk[0], 0, 1);
    dotAt(&disk[0], 32, 2);
    disk[0][64] = 0x00; // end-of-directory marker in slot 2
    fat[1] = eoc; // cluster 3 is a one-cluster chain
    try std.testing.expectEqual(ok, fs.dirmk.ra8_fs_rmdir(&m, "STUFF"));
    try std.testing.expectEqual(@as(usize, 1), erase_calls);
    try std.testing.expectEqual(@as(u8, 'D'), erase_name0);
    try std.testing.expectEqual(@as(u32, 0), fat[1]); // cluster 3 freed
}

test "rmdir refuses the root itself" {
    reset();
    resetDirmk();
    var m = fatmount();
    try std.testing.expectEqual(err_invalid_arg, fs.dirmk.ra8_fs_rmdir(&m, ""));
    try std.testing.expectEqual(@as(usize, 0), erase_calls);
}

test "rmdir refuses a file" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = ok;
    lookup_entry[0] = 'F';
    lookup_entry[11] = 0x20; // archive: a file, not a directory
    try std.testing.expectEqual(err_invalid_arg, fs.dirmk.ra8_fs_rmdir(&m, "STUFF"));
    try std.testing.expectEqual(@as(usize, 0), erase_calls);
    try std.testing.expectEqual(@as(usize, 0), fat_writes);
}

test "rmdir refuses a directory with no data cluster" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = ok;
    lookup_entry[11] = attr_dir;
    lookup_entry[26] = 0;
    try std.testing.expectEqual(err_protocol, fs.dirmk.ra8_fs_rmdir(&m, "STUFF"));
    try std.testing.expectEqual(@as(usize, 0), erase_calls);
}

test "rmdir refuses a non-empty directory: LFN remnants and deleted slots do not count" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = ok;
    lookup_entry[11] = attr_dir;
    lookup_entry[26] = 3;
    dotAt(&disk[0], 0, 1);
    dotAt(&disk[0], 32, 2);
    disk[0][64] = 0xE5; // deleted slot: skipped
    disk[0][96] = 'L'; // an LFN slot: its name bytes are live
    disk[0][96 + 11] = attr_lfn_v; // LFN slot: skipped
    disk[0][128] = 'A'; // a real entry: the directory is not empty
    disk[0][128 + 11] = 0x20;
    try std.testing.expectEqual(err_not_empty, fs.dirmk.ra8_fs_rmdir(&m, "STUFF"));
    try std.testing.expectEqual(@as(usize, 0), erase_calls);
    try std.testing.expectEqual(@as(usize, 0), fat_writes);
}

test "rmdir tolerates LFN remnants and deleted slots" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = ok;
    lookup_entry[11] = attr_dir;
    lookup_entry[26] = 3;
    dotAt(&disk[0], 0, 1);
    dotAt(&disk[0], 32, 2);
    disk[0][64] = 0xE5;
    disk[0][96] = 'L';
    disk[0][96 + 11] = attr_lfn_v;
    disk[0][128] = 0x00;
    fat[1] = eoc;
    try std.testing.expectEqual(ok, fs.dirmk.ra8_fs_rmdir(&m, "STUFF"));
    try std.testing.expectEqual(@as(usize, 1), erase_calls);
    try std.testing.expectEqual(@as(u32, 0), fat[1]); // cluster 3 freed
}

test "rmdir: a chain exhausted with no marker still reads as empty" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = ok;
    lookup_entry[11] = attr_dir;
    lookup_entry[26] = 3;
    @memset(&disk[0], 0xE5); // a full sector of deleted slots, no 0x00 marker
    fat[1] = eoc;
    try std.testing.expectEqual(ok, fs.dirmk.ra8_fs_rmdir(&m, "STUFF"));
    try std.testing.expectEqual(@as(usize, 1), erase_calls);
}

test "rmdir propagates a backend read failure while proving emptiness" {
    reset();
    resetDirmk();
    var m = fatmount();
    lookup_any_rc = ok;
    lookup_entry[11] = attr_dir;
    lookup_entry[26] = 3;
    read_err = err_io;
    try std.testing.expectEqual(err_io, fs.dirmk.ra8_fs_rmdir(&m, "STUFF"));
    try std.testing.expectEqual(@as(usize, 0), erase_calls);
    try std.testing.expectEqual(@as(usize, 0), fat_writes);
}

test "rmdir dispatch: idle mount and exFAT routing" {
    reset();
    resetDirmk();
    var m = fatmount();
    m.in_use = 0;
    try std.testing.expectEqual(err_invalid_state, fs.dirmk.ra8_fs_rmdir(&m, "STUFF"));
    m.in_use = 1;
    m.type = type_exfat;
    exfat_rc = err_not_found;
    try std.testing.expectEqual(err_not_found, fs.dirmk.ra8_fs_rmdir(&m, "STUFF"));
    try std.testing.expectEqual(@as(usize, 1), exfat_calls);
    try std.testing.expectEqual(@as(usize, 0), fat_writes);
}

export fn priv_dir_lookup_any(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, leaf: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32, out_entry: [*c]u8) callconv(.c) u16 {
    _ = m;
    _ = loc;
    _ = leaf;
    if (lookup_any_rc == ok) {
        out_lba.* = lookup_lba;
        out_off.* = lookup_off;
        @memcpy(out_entry[0..32], &lookup_entry);
    }
    return lookup_any_rc;
}

export fn priv_dir_reserve(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, leaf: [*c]const u8, plan: [*c]c.dir_insert_t) callconv(.c) u16 {
    _ = m;
    _ = loc;
    _ = leaf;
    _ = plan;
    return reserve_rc;
}

export fn priv_dir_commit(m: [*c]const c.ra8_fs_mount_t, plan: [*c]const c.dir_insert_t, tmpl: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32) callconv(.c) u16 {
    _ = m;
    _ = plan;
    if (commit_rc == ok) {
        out_lba.* = commit_lba;
        out_off.* = commit_off;
        @memcpy(commit_entry[0..32], tmpl[0..32]);
    }
    return commit_rc;
}

export fn priv_fat_entry_stamp_create(entry: [*c]u8) callconv(.c) void {
    _ = entry;
    stamp_calls += 1;
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

export fn priv_lfn_alias_basis(leaf: [*c]const u16, n: u32, tail: u32, out11: [*c]u8) callconv(.c) void {
    _ = .{ leaf, n };
    out11[0] = 'A';
    out11[1] = 'L';
    out11[2] = 'R';
    var v = tail;
    var i: usize = 3;
    while (i < 7) : (i += 1) {
        out11[i] = '0' + @as(u8, @intCast(v % 10));
        v /= 10;
    }
    out11[7] = '~';
    out11[8] = 'X';
    out11[9] = 'T';
    out11[10] = '0';
}
