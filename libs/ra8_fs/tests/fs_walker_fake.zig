//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! In-memory stand-ins for the C directory walkers the Zig units call. Every
//! test root that imports `ra8_fs` references this file so the test binary
//! links without the C library. The exFAT root is one cluster (5) of `slots`
//! entries; the FAT side is one 512-byte sector.

const std = @import("std");
const c = @import("ra8_fs").c;
const entry_bytes = 32;

pub const slots = 8;
pub var dir: [slots][entry_bytes]u8 = undefined;
pub var read_error: u16 = 0;
pub var written_index: ?u32 = null;
pub var written_cluster: u32 = 0;
pub var mount_store: c.ra8_fs_mount_t = std.mem.zeroes(c.ra8_fs_mount_t);

pub var sector: [512]u8 = undefined;
pub var walk_lba: u64 = 10;
pub var walk_fixed: u32 = 1;
pub var eps: u32 = 16;
pub var have83: u8 = 1;
pub var find83_err: u16 = 0;
pub var resolve_err: u16 = 0;
pub var sector_read_err: u16 = 0;
pub var sector_written: ?u64 = null;
pub var find_set_err: u16 = 0;
pub var set_count: u32 = 2;
pub var checksum_bytes: u32 = 0;
pub var stamped: [3]?*const c.ra8_fs_datetime_t = .{ null, null, null };
pub var stamp_calls: u32 = 0;

// Free-space side: a 32-entry FAT, a two-sector exFAT bitmap at LBA 100.
pub const bitmap_lba: u64 = 100;
pub var fat: [32]u32 = undefined;
pub var fat_err: u16 = 0;
pub var fat_reads: u32 = 0;
pub var free_cached: u32 = 0xFFFF_FFFF;
pub var bitmap: [1024]u8 = undefined;
pub var bitmap_err: u16 = 0;
pub var io_sector: [512]u8 = undefined;
pub var io_reads: u32 = 0;

// GPT side: LBAs 1..4 (header, then three entry sectors) and the scratch buffer.
pub var disk: [4][512]u8 = undefined;

// Open-for-write side: a two-handle slot pool and the stream walkers' knobs.
pub var file_pool: [2]c.ra8_fs_file_t = undefined;
pub var slots_left: u32 = 2;
pub var set_copies: bool = false;
pub var set_file: [entry_bytes]u8 = undefined;
pub var set_strm: [entry_bytes]u8 = undefined;
pub var name_units: u32 = 3;
pub var name_err: u16 = 0;
pub var bpb_err: u16 = 0;
pub var dir_cluster: u32 = 5;
pub var dir_lba: u64 = 4;
pub var lookup_err: u16 = 0;
pub var lookup_strm: [entry_bytes]u8 = @splat(0);
pub var lookup_attr: u8 = 0;
pub var parent_err: u16 = 0;
pub var link_err: u16 = 0;
pub var link_count: u32 = 3;
pub var free_calls: u32 = 0;
pub var free_err: u16 = 0;
pub var flush_calls: u32 = 0;
pub var flush_err: u16 = 0;
pub var fat_set_err: u16 = 0;
pub var fat_sets: u32 = 0;
pub var gave: u32 = 0;
pub var hint_low: u32 = 0xFFFF_FFFF;
export var g_fs_scratch: [4096]u8 = undefined;

pub fn mount() *const c.ra8_fs_mount_t {
    return &mount_store;
}

pub fn reset(fs_type: u8) void {
    for (&dir) |*e| e.* = @splat(0);
    dir[0][0] = 0x81; // allocation bitmap
    dir[1][0] = 0x82; // up-case table
    mount_store = std.mem.zeroes(c.ra8_fs_mount_t);
    mount_store.in_use = 1;
    mount_store.type = fs_type;
    mount_store.bytes_per_sector = 512;
    mount_store.sectors_per_cluster = 1;
    mount_store.root_cluster = 5;
    sector = @splat(0);
    read_error = 0;
    written_index = null;
    have83 = 1;
    find83_err = 0;
    resolve_err = 0;
    sector_read_err = 0;
    sector_written = null;
    find_set_err = 0;
    set_count = 2;
    checksum_bytes = 0;
    stamped = .{ null, null, null };
    stamp_calls = 0;
    fat = @splat(0);
    fat_err = 0;
    fat_reads = 0;
    free_cached = 0xFFFF_FFFF;
    bitmap = @splat(0);
    bitmap_err = 0;
    io_reads = 0;
    for (&disk) |*d| d.* = @splat(0);
    for (&file_pool) |*f| f.* = std.mem.zeroes(c.ra8_fs_file_t);
    slots_left = 2;
    set_copies = false;
    set_file = @splat(0);
    set_strm = @splat(0);
    name_units = 3;
    name_err = 0;
    parent_err = 0;
    link_err = 0;
    link_count = 3;
    free_calls = 0;
    free_err = 0;
    fat_set_err = 0;
    fat_sets = 0;
    gave = 0;
    hint_low = 0xFFFF_FFFF;
    walk_lba = 10;
    walk_fixed = 1;
    eps = 16;
    flush_calls = 0;
    flush_err = 0;
}

export fn priv_exfat_write_dir_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32, idx: u32, set: [*c]const u8, bytes: u32) callconv(.c) u16 {
    _ = m;
    written_cluster = cluster;
    written_index = idx;
    @memcpy(dir[idx][0..bytes], set[0..bytes]);
    return 0;
}

export fn priv_exfat_find_set(m: [*c]const c.ra8_fs_mount_t, d: [*c]const c.exfat_dir_t, path: [*c]const u8, pos: [*c]c.exfat_setpos_t, max_pos: u32, out_count: [*c]u32, file_copy: [*c]u8, strm_copy: [*c]u8) callconv(.c) u16 {
    _ = .{ m, path };
    if (find_set_err != 0) return find_set_err;
    if (set_copies) {
        @memcpy(file_copy[0..entry_bytes], &set_file);
        @memcpy(strm_copy[0..entry_bytes], &set_strm);
    }
    const n = @min(set_count, max_pos);
    for (0..n) |k| pos[k] = .{ .cluster = d.*.cluster, .index = @intCast(2 + k) };
    out_count.* = n;
    return 0;
}

export fn priv_exfat_set_checksum(set: [*c]const u8, bytes: u32) callconv(.c) u16 {
    checksum_bytes = bytes;
    return 0xA000 | @as(u16, set[4]);
}

export fn priv_wr16(p: [*c]u8, v: u16) callconv(.c) void {
    std.mem.writeInt(u16, p[0..2], v, .little);
}

export fn priv_resolve_parent(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out_parent: [*c]c.dir_loc_t, out_leaf: [*c][*c]const u8) callconv(.c) u16 {
    _ = m;
    if (resolve_err != 0) return resolve_err;
    out_parent.* = std.mem.zeroes(c.dir_loc_t);
    out_parent.*.is_root = 1;
    out_leaf.* = path;
    return 0;
}

export fn priv_path_to_83(path: [*c]const u8, out11: [*c]u8) callconv(.c) u8 {
    _ = .{ path, out11 };
    return have83;
}

export fn priv_dir_find(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, name83: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32, out_entry: [*c]u8) callconv(.c) u16 {
    _ = .{ m, loc, name83, out_entry };
    if (find83_err != 0) return find83_err;
    out_lba.* = 7;
    out_off.* = 64;
    return 0;
}

export fn priv_dir_eps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32 {
    _ = m;
    return eps;
}

export fn priv_dir_walk_init_loc(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, w: [*c]c.dir_walk_t) callconv(.c) void {
    _ = .{ m, loc };
    w.*.is_root_fixed = 1;
    w.*.fixed_remaining = walk_fixed;
    w.*.cluster = 0;
    w.*.sector_in_cluster = 0;
    w.*.cur_lba = walk_lba;
    w.*.entry_idx = 0;
    w.*.cluster_hops = 0;
}

export fn priv_dir_walk_next_sector(m: [*c]const c.ra8_fs_mount_t, w: [*c]c.dir_walk_t, out_eod: [*c]u8) callconv(.c) u16 {
    _ = m;
    if (w.*.fixed_remaining == 0) {
        out_eod.* = 1;
        return 0;
    }
    w.*.fixed_remaining -= 1;
    w.*.cur_lba += 1;
    out_eod.* = 0;
    return 0;
}

export fn priv_byte_copy(dst: [*c]u8, src: [*c]const u8, n: u32) callconv(.c) void {
    @memcpy(dst[0..n], src[0..n]);
}

/// Lay a real long-name directory image into the sector buffer at byte `at`:
/// one LFN sub-entry per 13 UTF-16 units of `name` (highest group first,
/// 0x40 flag on the physically first), then the 8.3 entry `sfn11` with the
/// archive attribute. What a mounted FAT directory actually looks like.
pub fn layLongName(name: []const u8, comptime sfn11: *const [11]u8, at: usize) void {
    const lf = @import("ra8_fs").lfn;
    const csum = lf.priv_sfn_checksum(sfn11);
    const groups: u32 = @intCast((name.len + 12) / 13);
    var units: [247]u16 = @splat(0);
    for (name, 0..) |ch, i| units[i] = ch;
    var g: u32 = 0;
    while (g < groups) : (g += 1) {
        var ent: [32]u8 = @splat(0);
        lf.priv_lfn_fill_slot(&ent, &units, @intCast(name.len), groups - g, @intFromBool(g == 0), csum);
        @memcpy(sector[at + g * 32 ..][0..32], &ent);
    }
    @memcpy(sector[at + groups * 32 ..][0..11], sfn11);
    sector[at + groups * 32 + 11] = 0x20;
}

export fn priv_sec_walk() callconv(.c) [*c]u8 {
    return &sector;
}

export fn priv_read_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.c) u16 {
    _ = m;
    if (sector_read_err != 0) return sector_read_err;
    if (lba == dir_lba) {
        for (0..slots) |i| {
            @memcpy(buf[i * entry_bytes ..][0..entry_bytes], &dir[i]);
        }
        for (slots..eps) |i| {
            buf[i * entry_bytes ..][0..entry_bytes].* = @splat(0x85);
        }
        return 0;
    }
    if (lba >= 1 and lba < 5) {
        @memcpy(buf[0..512], &disk[@intCast(lba - 1)]);
        return 0;
    }
    if (lba >= bitmap_lba and lba < bitmap_lba + 2) {
        const at: usize = @intCast((lba - bitmap_lba) * 512);
        @memcpy(buf[0..512], bitmap[at..][0..512]);
        io_reads += 1;
    }
    return 0;
}

export fn priv_write_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]const u8) callconv(.c) u16 {
    _ = .{ m, buf };
    sector_written = lba;
    return 0;
}

export fn priv_fat_entry_apply_attr(entry: [*c]u8, set_mask: u8, clear_mask: u8) callconv(.c) void {
    entry[11] = (entry[11] & ~clear_mask) | set_mask;
}

fn stamp(entry: [*c]u8, create: [*c]const c.ra8_fs_datetime_t, modify: [*c]const c.ra8_fs_datetime_t, access: [*c]const c.ra8_fs_datetime_t) void {
    stamped = .{ create, modify, access };
    stamp_calls += 1;
    if (modify) |mt| entry[22] = mt.*.minute; // marker byte the tests read back
}

export fn priv_fat_entry_set_times(entry: [*c]u8, create: [*c]const c.ra8_fs_datetime_t, modify: [*c]const c.ra8_fs_datetime_t, access: [*c]const c.ra8_fs_datetime_t) callconv(.c) void {
    stamp(entry, create, modify, access);
}

export fn priv_exfat_file_set_times(entry: [*c]u8, create: [*c]const c.ra8_fs_datetime_t, modify: [*c]const c.ra8_fs_datetime_t, access: [*c]const c.ra8_fs_datetime_t) callconv(.c) void {
    stamp(entry, create, modify, access);
}

export fn priv_sec_io() callconv(.c) [*c]u8 {
    return &io_sector;
}

export fn priv_bps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32 {
    _ = m;
    return 512;
}

export fn priv_cluster_bytes(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32 {
    return @as(u32, m.*.bytes_per_sector) * @as(u32, m.*.sectors_per_cluster);
}

export fn priv_cluster_to_lba(m: [*c]const c.ra8_fs_mount_t, clus: u32) callconv(.c) u64 {
    _ = m;
    if (clus == dir_cluster) return dir_lba;
    return if (clus == 3) bitmap_lba else @as(u64, clus) * 8;
}

export fn priv_exfat_find_bitmap(m: [*c]const c.ra8_fs_mount_t, out_clus: [*c]u32, out_len: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_clus.* = 3;
    out_len.* = 1024;
    return bitmap_err;
}

export fn priv_fat_get(m: [*c]const c.ra8_fs_mount_t, clus: u32, out: [*c]u32) callconv(.c) u16 {
    _ = m;
    fat_reads += 1;
    if (fat_err != 0) return fat_err;
    out.* = fat[clus];
    return 0;
}

/// The exFAT rotate-add checksum (spec sec 6.3.3), standing in for the C one.
export fn priv_exfat_csum32(cs: u32, buf: [*c]const u8, len: u32) callconv(.c) u32 {
    var sum = cs;
    for (buf[0..len]) |b| sum = std.math.rotr(u32, sum, 1) +% b;
    return sum;
}

export fn priv_alloc_file_slot() callconv(.c) [*c]c.ra8_fs_file_t {
    if (slots_left == 0) return null;
    slots_left -= 1;
    return &file_pool[slots_left];
}

export fn priv_is_eoc(m: [*c]const c.ra8_fs_mount_t, value: u32) callconv(.c) u8 {
    _ = m;
    return if (value >= 0x0FFF_FFF8) 1 else 0;
}

export fn priv_exfat_resolve_parent(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out_parent: [*c]c.exfat_dir_t, out_leaf: [*c][*c]const u8) callconv(.c) u16 {
    _ = m;
    if (parent_err != 0) return parent_err;
    out_parent.* = std.mem.zeroes(c.exfat_dir_t);
    out_parent.*.cluster = 5;
    out_leaf.* = path;
    return 0;
}

export fn priv_exfat_link(m: [*c]const c.ra8_fs_mount_t, d: [*c]const c.exfat_dir_t, name: [*c]const u16, nlen: u32, out_head: [*c]c.exfat_setpos_t, out_count: [*c]u32) callconv(.c) u16 {
    _ = .{ m, name, nlen };
    if (link_err != 0) return link_err;
    out_head.* = .{ .cluster = d.*.cluster, .index = 6 };
    out_count.* = link_count;
    return 0;
}

export fn priv_exfat_free_clusters(m: [*c]const c.ra8_fs_mount_t, strm: [*c]const u8) callconv(.c) u16 {
    _ = .{ m, strm };
    free_calls += 1;
    return free_err;
}

export fn priv_exfat_flush_set(file: [*c]c.ra8_fs_file_t) callconv(.c) u16 {
    _ = file;
    flush_calls += 1;
    return flush_err;
}

export fn priv_fat_set(m: [*c]const c.ra8_fs_mount_t, clus: u32, value: u32) callconv(.c) u16 {
    _ = m;
    if (fat_set_err != 0) return fat_set_err;
    fat_sets += 1;
    fat[clus] = value;
    return 0;
}

export fn priv_rd16(p: [*c]const u8) callconv(.c) u16 {
    return @as(u16, p[0]) | (@as(u16, p[1]) << 8);
}

export fn priv_rd32(p: [*c]const u8) callconv(.c) u32 {
    return @as(u32, priv_rd16(p)) | (@as(u32, priv_rd16(p + 2)) << 16);
}

export fn priv_rd64(p: [*c]const u8) callconv(.c) u64 {
    return @as(u64, priv_rd32(p)) | (@as(u64, priv_rd32(p + 4)) << 32);
}

export fn priv_parse_bpb_into_mount(m: [*c]c.ra8_fs_mount_t) callconv(.c) u16 {
    _ = m;
    return bpb_err;
}

export fn priv_exfat_lookup(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out_strm: [*c]u8, out_attr: [*c]u8) callconv(.c) u16 {
    _ = m;
    _ = path;
    if (lookup_err != 0) return lookup_err;
    @memcpy(out_strm[0..entry_bytes], &lookup_strm);
    out_attr.* = lookup_attr;
    return 0;
}
