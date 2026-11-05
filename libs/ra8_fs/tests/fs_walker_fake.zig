//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! In-memory stand-ins for the C units the Zig units still call, plus the
//! fake block backend and directory fixtures the Zig roots test against.
//! Every test root that imports `ra8_fs` references this file so the test
//! binary links without the C library. The exFAT root is one cluster (5) of
//! `slots` entries; the FAT side is one 512-byte sector.

const std = @import("std");
const al = @import("ra8_fs").alloc;
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
pub var sector_read_err: u16 = 0;
pub var sector_write_err: u16 = 0;
pub var sector_written: ?u64 = null;
pub var find_set_err: u16 = 0;
pub var set_count: u32 = 2;
pub var checksum_bytes: u32 = 0;

// Free-space side: a 32-entry FAT, a two-sector exFAT bitmap at LBA 100.
pub const bitmap_lba: u64 = 100;
/// The FAT lives at its own fixed LBA so no other fake range collides with it.
pub const fat_lba: u64 = 900;
pub const fat_secs: u64 = 2;
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
    mount_store.backend.read_block = read_block_backend;
    mount_store.backend.write_block = write_block_backend;
    mount_store.bytes_per_sector = 512;
    mount_store.sectors_per_cluster = 1;
    // The real entry layer maps cluster -> LBA: cluster 5 (root) must
    // land on dir_lba, so the cluster heap starts one LBA before it.
    mount_store.first_data_lba = 1;
    // FAT32 directories root on the cluster chain (root_cluster 5), so the
    // heap start moves so cluster 5's LBA is walk_lba, where the dir buffer
    // is served. exFAT fixtures keep first_data_lba 1.
    if (fs_type == c.k_ra8_fs_type_fat32) mount_store.first_data_lba = walk_lba - 3;
    mount_store.first_fat_lba = fat_lba;
    mount_store.fat_size_sectors = fat_secs;
    mount_store.num_fats = 1;
    al.priv_fat_cache_reset();
    mount_store.root_cluster = 5;
    sector = @splat(0);
    read_error = 0;
    written_index = null;
    sector_read_err = 0;
    sector_write_err = 0;
    sector_written = null;
    find_set_err = 0;
    set_count = 2;
    checksum_bytes = 0;
    fat = @splat(0);
    if (fs_type == c.k_ra8_fs_type_fat32) {
        fat[5] = 0xFFFF_FFF8; // the FAT32 root chain ends after its first cluster
    }
    fat_err = 0;
    fat_set_err = 0;
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
    // The walkers are Zig now: the fixed-root region starts at walk_lba and
    // spans walk_fixed sectors (16 entries per 512-byte sector).
    mount_store.first_root_lba = walk_lba;
    mount_store.root_entries = walk_fixed * 16;
    dir_lba = 4;
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

/// Lay a plain 8.3 directory entry into the sector buffer at slot `at`: the
/// 11-byte name field `sfn11`, attribute, and first cluster. What the 8.3
/// find matches against on a live directory image.
pub fn lay83(at: usize, sfn11: *const [11]u8, attr: u8, first_cluster: u32) void {
    sector[at * entry_bytes ..][0..entry_bytes].* = @splat(0);
    @memcpy(sector[at * entry_bytes ..][0..11], sfn11);
    sector[at * entry_bytes + 11] = attr;
    sector[at * entry_bytes + 26] = @truncate(first_cluster);
    sector[at * entry_bytes + 27] = @truncate(first_cluster >> 8);
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

/// Serve the FAT array as raw sectors. The array is indexed by cluster
/// directly (`fat[cluster]`), so entry `i` lands at byte `i * entry_size`.
fn fatToSector(buf: [*c]u8) void {
    const t = mount_store.type;
    if (t == c.k_ra8_fs_type_fat12) {
        for (fat, 0..) |v, i| {
            const off = i + i / 2;
            if (i % 2 == 0) {
                buf[off] = @truncate(v);
                buf[off + 1] = (buf[off + 1] & 0x0F) | @as(u8, @truncate(v >> 8)) << 4;
            } else {
                buf[off] = (buf[off] & 0x0F) | @as(u8, @truncate(v)) << 4;
                buf[off + 1] = @truncate(v >> 4);
            }
        }
        return;
    }
    const esz: usize = if (t == c.k_ra8_fs_type_fat16) 2 else 4;
    for (fat, 0..) |v, i| {
        const at = i * esz;
        buf[at] = @truncate(v);
        buf[at + 1] = @truncate(v >> 8);
        if (esz == 4) {
            buf[at + 2] = @truncate(v >> 16);
            buf[at + 3] = @truncate(v >> 24);
        }
    }
}

/// The write side of the same mapping: parse a FAT sector back into the array.
fn fatFromSector(buf: [*c]u8) void {
    const t = mount_store.type;
    if (t == c.k_ra8_fs_type_fat12) {
        for (0..fat.len) |i| {
            const off = i + i / 2;
            const raw: u16 = @as(u16, buf[off]) | (@as(u16, buf[off + 1]) << 8);
            fat[i] = if (i % 2 == 0) raw & 0x0FFF else raw >> 4;
        }
        return;
    }
    const esz: usize = if (t == c.k_ra8_fs_type_fat16) 2 else 4;
    for (0..fat.len) |i| {
        const at = i * esz;
        var v: u32 = @as(u32, buf[at]) | (@as(u32, buf[at + 1]) << 8);
        if (esz == 4) v |= (@as(u32, buf[at + 2]) << 16) | (@as(u32, buf[at + 3]) << 24);
        fat[i] = v;
    }
}

fn read_block_backend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (sector_read_err != 0) return sector_read_err;
    const fat_window = fat_lba + fat_secs * @max(mount_store.num_fats, 1);
    if (lba >= fat_lba and lba < fat_window) {
        fat_reads += 1;
        if (fat_err != 0) return fat_err;
        fatToSector(buf);
        return 0;
    }
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

fn write_block_backend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    _ = .{ ctx, count };
    if (lba >= fat_lba and lba < fat_lba + fat_secs * @max(mount_store.num_fats, 1)) {
        if (fat_set_err != 0) return fat_set_err;
        // Every FAT copy maps onto the same array: copies must be identical.
        fatFromSector(@constCast(buf));
        fat_sets += 1;
        sector_written = lba;
        return 0;
    }
    if (sector_write_err != 0) return sector_write_err;
    sector_written = lba;
    return 0;
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

export fn priv_sec_io() callconv(.c) [*c]u8 {
    return &io_sector;
}
export fn priv_exfat_find_bitmap(m: [*c]const c.ra8_fs_mount_t, out_clus: [*c]u32, out_len: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_clus.* = bitmap_lba - 1 + 2;
    out_len.* = 1024;
    return bitmap_err;
}
/// The exFAT rotate-add checksum (spec sec 6.3.3), standing in for the C one.
export fn priv_exfat_csum32(cs: u32, buf: [*c]const u8, len: u32) callconv(.c) u32 {
    var sum = cs;
    for (buf[0..len]) |b| sum = std.math.rotr(u32, sum, 1) +% b;
    return sum;
}

// --- dir lookup/insert fakes for the FAT open path (RA8FW-1092) ---

export fn priv_alloc_file_slot() callconv(.c) [*c]c.ra8_fs_file_t {
    if (slots_left == 0) return null;
    slots_left -= 1;
    return &file_pool[slots_left];
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
/// FAT first-cluster decode, standing in for the C one in ra8_fs_fat_file.c.
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
