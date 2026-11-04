//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT read side (RA8FW-1087): volume parse, the up-case walk, the
//! directory cursor machinery, flat name matching, and the read open,
//! against this file's own fakes.

const std = @import("std");
const al = @import("ra8_fs").alloc;
const fs = @import("ra8_fs");
const ex = fs.exfat_read;
const upcase = fs.upcase;
const c = fs.c;

const bps: u32 = 512;
const spc: u32 = 1;
const cluster_bytes: u32 = bps * spc;
const per_cluster: u32 = cluster_bytes / 32;
const data_lba: u64 = 1000;
const eoc: u32 = 0xFFFFFFF8;
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
    fat_read_err = 0;
    pool_used = 0;
    bpb_result = 0xAAAA;
    lookup_err = 0;
    lookup_strm = @splat(0);
    lookup_attr = 0;
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

test "parse_volume reads the exFAT VBR into the mount" {
    reset();
    var vbr: [bps]u8 = @splat(0);
    @memcpy(vbr[3..11], "EXFAT   ");
    vbr[0x6C] = 9; // 512-byte sectors
    vbr[0x6D] = 0; // one sector per cluster
    vbr[0x6E] = 1;
    wr32(&vbr, 0x50, 64); // FAT lba
    wr32(&vbr, 0x54, 32); // FAT length
    wr32(&vbr, 0x58, 1000); // heap lba
    wr32(&vbr, 0x5C, 1234); // cluster count
    wr32(&vbr, 0x60, 2); // root cluster
    wr64(&vbr, 72, 9999); // volume length
    @memcpy(g_fs_scratch[0..bps], &vbr);
    var m = mount();
    try std.testing.expectEqual(ok, ex.priv_parse_volume(&m));
    try std.testing.expectEqual(c.k_ra8_fs_type_exfat, m.type);
    try std.testing.expectEqual(@as(u32, 1), m.sectors_per_cluster);
    try std.testing.expectEqual(@as(u32, 1), m.num_fats);
    try std.testing.expectEqual(@as(u32, 32), m.fat_size_sectors);
    try std.testing.expectEqual(@as(u64, 64), m.first_fat_lba);
    try std.testing.expectEqual(@as(u64, 1000), m.first_data_lba);
    try std.testing.expectEqual(@as(u32, 2), m.root_cluster);
    try std.testing.expectEqual(@as(u32, 1234), m.count_of_clusters);
    try std.testing.expectEqual(@as(u64, 9999), m.total_sectors);
    try std.testing.expectEqual(@as(u8, 0), m.exfat_upcase_ok);
}

test "parse_volume falls back to the BPB when the signature is not exFAT" {
    reset();
    var m = mount();
    try std.testing.expectEqual(@as(u16, 0xAAAA), ex.priv_parse_volume(&m));
}

test "the shift guard rejects a volume sized for another medium" {
    reset();
    var vbr: [bps]u8 = @splat(0);
    @memcpy(vbr[3..11], "EXFAT   ");
    vbr[0x6C] = 12; // 4096-byte sectors on a 512-byte medium
    @memcpy(g_fs_scratch[0..bps], &vbr);
    var m = mount();
    try std.testing.expectEqual(err_not_supported, ex.priv_parse_volume(&m));

    vbr[0x6C] = 8; // below the 512-byte floor
    @memcpy(g_fs_scratch[0..bps], &vbr);
    try std.testing.expectEqual(err_not_supported, ex.priv_parse_volume(&m));
}

test "the up-case walk marks a table this build embeds" {
    reset();
    var m = mount();
    disk[0][0] = 0x82;
    wr32(&disk[0], 4, upcase.priv_exfat_upcase_checksum());
    disk[0][32] = 0; // end-of-directory
    ex.priv_exfat_upcase_verify(&m);
    try std.testing.expectEqual(@as(u8, 1), m.exfat_upcase_ok);

    disk[0] = @splat(0);
    disk[0][0] = 0x82;
    wr32(&disk[0], 4, 0xDEADBEEF);
    disk[0][32] = 0;
    m.exfat_upcase_ok = 1; // proven wrong again by the walk
    ex.priv_exfat_upcase_verify(&m);
    try std.testing.expectEqual(@as(u8, 0), m.exfat_upcase_ok);
}

test "the up-case walk ends at end-of-directory without a table" {
    reset();
    var m = mount();
    disk[0][0] = 0;
    ex.priv_exfat_upcase_verify(&m);
    try std.testing.expectEqual(@as(u8, 0), m.exfat_upcase_ok);

    read_err = 0x202;
    ex.priv_exfat_upcase_verify(&m);
    try std.testing.expectEqual(@as(u8, 0), m.exfat_upcase_ok);
}

test "dir_root seeds the root cursor at the root cluster" {
    reset();
    var m = mount();
    var out: c.exfat_dir_t = undefined;
    ex.priv_exfat_dir_root(&m, &out);
    try std.testing.expectEqual(@as(u32, 2), out.cluster);
    try std.testing.expectEqual(@as(u32, 0), out.contig_end);
    try std.testing.expectEqual(@as(u32, 0), out.self_cluster);
    try std.testing.expectEqual(@as(u32, 0), out.self_index);
}

test "dir_from_set reads a no-fat-chain run from the stream entry" {
    reset();
    var m = mount();
    var out: c.exfat_dir_t = undefined;
    var strm: [32]u8 = @splat(0);
    strm[1] = 0x02; // NoFatChain
    wr32(&strm, 0x14, 7);
    wr64(&strm, 0x18, 1000); // two clusters of 512 bytes
    ex.priv_exfat_dir_from_set(&m, &strm, &out);
    try std.testing.expectEqual(@as(u32, 7), out.cluster);
    try std.testing.expectEqual(@as(u32, 9), out.contig_end);

    strm[1] = 0; // FAT-chained: the extent is unbounded
    ex.priv_exfat_dir_from_set(&m, &strm, &out);
    try std.testing.expectEqual(@as(u32, 0), out.contig_end);
}

test "step_cluster walks a contiguous run and stops at its end" {
    reset();
    var m = mount();
    var next: u32 = 0;
    try std.testing.expectEqual(ok, ex.priv_exfat_step_cluster(&m, 7, 9, &next));
    try std.testing.expectEqual(@as(u32, 8), next);
    try std.testing.expectEqual(err_not_found, ex.priv_exfat_step_cluster(&m, 8, 9, &next));

    fat[0] = 3; // cluster 2 -> 3 through the FAT
    try std.testing.expectEqual(ok, ex.priv_exfat_step_cluster(&m, 2, 0, &next));
    try std.testing.expectEqual(@as(u32, 3), next);
    fat[0] = eoc;
    al.priv_fat_cache_reset(); // the real entry layer caches the FAT sector
    try std.testing.expectEqual(err_not_found, ex.priv_exfat_step_cluster(&m, 2, 0, &next));
    fat_read_err = 0x303;
    al.priv_fat_cache_reset();
    try std.testing.expectEqual(@as(u16, 0x303), ex.priv_exfat_step_cluster(&m, 2, 0, &next));
}

test "next_entry walks entries and crosses a cluster boundary" {
    reset();
    var m = mount();
    fat[0] = 3; // cluster 2 -> 3
    for (0..per_cluster) |i| disk[0][i * 32] = @intCast(i);
    disk[1][0] = 0xAA; // first entry of cluster 3

    var cur: c.exfat_cursor_t = std.mem.zeroes(c.exfat_cursor_t);
    cur.cluster = 2;
    var e: [32]u8 = undefined;
    for (0..per_cluster) |i| {
        try std.testing.expectEqual(ok, ex.priv_exfat_next_entry(&m, &cur, &e));
        try std.testing.expectEqual(@as(u8, @intCast(i)), e[0]);
    }
    // The step is lazy: the post-increment ends the cluster full, and the
    // crossing happens at the top of the next call.
    try std.testing.expectEqual(@as(u32, per_cluster), cur.entry_in_cluster);
    try std.testing.expectEqual(@as(u32, 2), cur.cluster);
    try std.testing.expectEqual(ok, ex.priv_exfat_next_entry(&m, &cur, &e));
    try std.testing.expectEqual(@as(u8, 0xAA), e[0]);
    try std.testing.expectEqual(@as(u32, 1), cur.entry_in_cluster);
    try std.testing.expectEqual(@as(u32, 3), cur.cluster);
    try std.testing.expectEqual(@as(u32, per_cluster + 1), cur.scanned);

    read_err = 0x202;
    try std.testing.expectEqual(@as(u16, 0x202), ex.priv_exfat_next_entry(&m, &cur, &e));
}

test "name matching folds the needle and the on-disk name the same way" {
    reset();
    var m = mount();
    disk[0][0] = 0x85; // File entry
    disk[0][4] = 0x20; // archive attribute
    strEntry(&disk[0], 1, 5, 0, 777, 777);
    nameEntry(&disk[0], 2, "NOTES.TXT");
    disk[0][3 * 32] = 0; // end-of-directory

    const dir: c.exfat_dir_t = .{ .cluster = 2, .contig_end = 0, .self_cluster = 0, .self_index = 0 };
    var strm: [32]u8 = @splat(0);
    var attr: u8 = 0;
    try std.testing.expectEqual(ok, ex.priv_exfat_find(&m, &dir, "/notes.txt", &strm, &attr));
    try std.testing.expectEqual(@as(u8, 0x20), attr);
    try std.testing.expectEqual(@as(u32, 5), fs.prim.priv_rd32(strm[0x14..].ptr));
    try std.testing.expectEqual(@as(u64, 777), fs.prim.priv_rd64(strm[0x18..].ptr));
}

test "a set naming another entry is skipped and the scan continues" {
    reset();
    var m = mount();
    disk[0][0] = 0x85; // first set: a directory named OTHER.TXT
    disk[0][4] = 0x10;
    strEntry(&disk[0], 1, 7, 0, 1, 1);
    nameEntry(&disk[0], 2, "OTHER.TXT");
    disk[0][3 * 32] = 0x85; // second File entry
    disk[0][3 * 32 + 4] = 0x20;
    strEntry(&disk[0], 4, 5, 0, 777, 777);
    nameEntry(&disk[0], 5, "NOTES.TXT");
    disk[0][6 * 32] = 0;

    const dir: c.exfat_dir_t = .{ .cluster = 2, .contig_end = 0, .self_cluster = 0, .self_index = 0 };
    var strm: [32]u8 = @splat(0);
    var attr: u8 = 0;
    try std.testing.expectEqual(ok, ex.priv_exfat_find(&m, &dir, "notes.txt", &strm, &attr));
    try std.testing.expectEqual(@as(u8, 0x20), attr);
}

test "find reports not_found at end-of-directory and on backend errors" {
    reset();
    var m = mount();
    disk[0][0] = 0;
    const dir: c.exfat_dir_t = .{ .cluster = 2, .contig_end = 0, .self_cluster = 0, .self_index = 0 };
    var strm: [32]u8 = @splat(0);
    var attr: u8 = 0;
    try std.testing.expectEqual(err_not_found, ex.priv_exfat_find(&m, &dir, "x", &strm, &attr));

    disk[0][0] = 0x85;
    disk[0][32] = 0;
    read_err = 0x202;
    try std.testing.expectEqual(@as(u16, 0x202), ex.priv_exfat_find(&m, &dir, "x", &strm, &attr));
}

test "a needle past the name cap reads as absent" {
    reset();
    var m = mount();
    const dir: c.exfat_dir_t = .{ .cluster = 2, .contig_end = 0, .self_cluster = 0, .self_index = 0 };
    var strm: [32]u8 = @splat(0);
    var attr: u8 = 0;
    var long_buf: [66]u8 = @splat('a');
    long_buf[65] = 0;
    const long = long_buf[0..65 :0];
    try std.testing.expectEqual(err_not_found, ex.priv_exfat_find(&m, &dir, long ++ "\x00", &strm, &attr));

    var units: [c.k_exfat_name_cap]u16 = @splat(0);
    var n: u32 = 0;
    try std.testing.expectEqual(@as(u16, 0xBEEF), ex.priv_exfat_needle_units(&m, long ++ "\x00", &units, &n, 0xBEEF));
}

test "name_to_units refuses a non-ASCII name without a verified table" {
    reset();
    var m = mount();
    var units: [c.k_exfat_name_cap]u16 = @splat(0);
    var n: u32 = 0;
    try std.testing.expectEqual(err_not_supported, ex.priv_exfat_name_to_units(&m, "h\u{e9}llo\x00", &units, &n));
    try std.testing.expectEqual(@as(u32, 0), n);

    m.exfat_upcase_ok = 1;
    try std.testing.expectEqual(ok, ex.priv_exfat_name_to_units(&m, "h\u{e9}llo\x00", &units, &n));
    try std.testing.expectEqual(@as(u32, 5), n);
}

test "name_chunk_eq agrees with units past the needle end" {
    reset();
    const needle = needleFor("ab");
    var nm: [32]u8 = @splat(0);
    nm[2] = 'A';
    nm[4] = 'B';
    nm[6] = 'C';
    try std.testing.expectEqual(@as(u8, 1), ex.priv_exfat_name_chunk_eq(&nm, &needle, 0, 2));
    try std.testing.expectEqual(@as(u8, 1), ex.priv_exfat_name_chunk_eq(&nm, &needle, 2, 2)); // padding agrees
    try std.testing.expectEqual(@as(u8, 0), ex.priv_exfat_name_chunk_eq(&nm, &needle, 1, 2)); // B vs A mismatches
}

test "the read open rejects a directory" {
    reset();
    var m = mount();
    lookup_attr = 0x10;
    var out: ?*ex.File = null;
    try std.testing.expectEqual(err_invalid_arg, ex.priv_exfat_open(&m, "/d", c.k_ra8_fs_mode_read, &out));
}

test "the read open seeds the handle from the stream entry" {
    reset();
    var m = mount();
    lookup_attr = 0x20;
    lookup_strm[1] = 0x02;
    wr32(&lookup_strm, 0x14, 5);
    wr64(&lookup_strm, 0x18, 777);
    wr64(&lookup_strm, 8, 700);
    var out: ?*ex.File = null;
    try std.testing.expectEqual(ok, ex.priv_exfat_open(&m, "/notes.txt", c.k_ra8_fs_mode_read, &out));
    const f = out.?;
    try std.testing.expectEqual(@as(u32, 5), f.first_cluster);
    try std.testing.expectEqual(@as(u32, 5), f.cur_cluster);
    try std.testing.expectEqual(@as(u64, 777), f.size_bytes);
    try std.testing.expectEqual(@as(u64, 700), f.valid_bytes);
    try std.testing.expectEqual(@as(u8, 1), f.no_fat_chain);
    try std.testing.expectEqual(@as(u8, 1), f.in_use);
    try std.testing.expectEqual(@as(u8, 0), f.dirty);
    try std.testing.expectEqual(@as(u64, 0), f.offset);
    try std.testing.expectEqual(c.k_ra8_fs_mode_read, f.mode);

    pool_used = pool.len; // no slot free
    var out2: ?*ex.File = null;
    try std.testing.expectEqual(err_no_mem, ex.priv_exfat_open(&m, "/notes.txt", c.k_ra8_fs_mode_read, &out2));
}

test "the read open propagates lookup failures" {
    reset();
    var m = mount();
    lookup_err = err_not_found;
    var out: ?*ex.File = null;
    try std.testing.expectEqual(err_not_found, ex.priv_exfat_open(&m, "/gone.txt", c.k_ra8_fs_mode_read, &out));
}

test "open routes writing modes to the openw path" {
    reset();
    var m = mount();
    resolve_err = 0x4242;
    var out: ?*ex.File = null;
    try std.testing.expectEqual(@as(u16, 0x4242), ex.priv_exfat_open(&m, "/w", c.k_ra8_fs_mode_write, &out));
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
    if (lba >= fat_lba and lba < fat_lba + fat_secs) {
        fatFromSector(@constCast(buf));
        return ok;
    }
    return 0;
}

// ---- stand-ins for the five C helpers the open/close port (RA8FW-1092) added;
// the open/create path is not exercised here, so they only satisfy the linker
var resolve_err: u16 = 0;

fn priv_dir_reserve(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, leaf: [*c]const u8, plan: [*c]c.dir_insert_t) callconv(.c) u16 {
    _ = m;
    _ = loc;
    _ = leaf;
    _ = plan;
    return 0;
}

fn priv_dir_commit(m: [*c]const c.ra8_fs_mount_t, plan: [*c]const c.dir_insert_t, tmpl: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32) callconv(.c) u16 {
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

fn priv_lfn_alias_basis(leaf: [*c]const u16, n: u32, tail: u32, out11: [*c]u8) callconv(.c) void {
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

export fn priv_alloc_eoc_cluster(m: [*c]const u8, out_c: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_c.* = 6;
    return 0;
}
