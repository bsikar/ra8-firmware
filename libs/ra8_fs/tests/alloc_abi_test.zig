const std = @import("std");
const fs = @import("ra8_fs");
const ex = fs.exfat_read;
const al = fs.alloc;
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
var read_calls: u32 = 0;
var write_calls: u32 = 0;
var write_err: u16 = 0;
var find_bitmap_calls: u32 = 0;
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
    write_err = 0;
    read_calls = 0;
    write_calls = 0;
    find_bitmap_calls = 0;
    @memset(&g_fs_scratch, 0);
}

export fn priv_rd16(p: [*]const u8) callconv(.c) u16 {
    return @as(u16, p[0]) | (@as(u16, p[1]) << 8);
}

export fn priv_rd32(p: [*]const u8) callconv(.c) u32 {
    return @as(u32, priv_rd16(p)) | (@as(u32, priv_rd16(p + 2)) << 16);
}

export fn priv_rd64(p: [*]const u8) callconv(.c) u64 {
    return @as(u64, priv_rd32(p)) | (@as(u64, priv_rd32(p + 4)) << 32);
}

export fn priv_byte_copy(dst: [*]u8, src: [*]const u8, n: u32) callconv(.c) void {
    @memcpy(dst[0..n], src[0..n]);
}

export fn priv_cluster_bytes(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32 {
    return m.*.sectors_per_cluster * m.*.bytes_per_sector;
}

export fn priv_bps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32 {
    return m.*.bytes_per_sector;
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

export fn priv_is_eoc(m: [*c]const c.ra8_fs_mount_t, value: u32) callconv(.c) u8 {
    _ = m;
    return if (value >= eoc) 1 else 0;
}

var sec_buf: [c.k_ra8_fs_sector_max]u8 = @splat(0);

export fn priv_sec_io() callconv(.c) [*c]u8 {
    return &sec_buf;
}

export fn priv_read_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.c) u16 {
    _ = m;
    if (read_err != 0) return read_err;
    read_calls += 1;
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

// ---------------------------------------------------------------- tests
// Link stubs for the other root units this binary analyzes (never exercised
// here): the shared root pulls every abi module into every test root.
var walk_sector: [512]u8 = @splat(0);

export fn priv_dir_eps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32 {
    _ = m;
    return 16;
}

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
    find_bitmap_calls += 1;
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

export fn priv_fat_entry_apply_attr(entry: [*c]u8, set_mask: u8, clear_mask: u8) callconv(.c) void {
    entry[11] = (entry[11] & ~clear_mask) | set_mask;
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

export fn priv_resolve_parent(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out_parent: [*c]c.dir_loc_t, out_leaf: [*c][*c]const u8) callconv(.c) u16 {
    _ = m;
    out_parent.* = std.mem.zeroes(c.dir_loc_t);
    out_leaf.* = path;
    return 0;
}

export fn priv_sec_walk() callconv(.c) [*c]u8 {
    return &walk_sector;
}

export fn priv_wr16(p: [*c]u8, v: u16) callconv(.c) void {
    std.mem.writeInt(u16, p[0..2], v, .little);
}

export fn priv_write_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]const u8) callconv(.c) u16 {
    _ = m;
    if (write_err != 0) return write_err;
    const i: usize = @intCast(lba - data_lba);
    @memcpy(disk[i][0..bps], buf[0..bps]);
    write_calls += 1;
    return 0;
}

// ---------------------------------------------------------------------------
// alloc tests
// ---------------------------------------------------------------------------

fn mkFat32() c.ra8_fs_mount_t {
    var m = std.mem.zeroes(c.ra8_fs_mount_t);
    m.type = c.k_ra8_fs_type_fat32;
    m.bytes_per_sector = bps;
    m.sectors_per_cluster = spc;
    m.first_fat_lba = 100;
    m.fat_size_sectors = 10;
    m.reserved_sectors = 4;
    m.count_of_clusters = 100;
    m.first_data_lba = 20;
    return m;
}

fn put16(sector: *[bps]u8, off: usize, v: u16) void {
    std.mem.writeInt(u16, sector[off..][0..2], v, .little);
}

fn put32(sector: *[bps]u8, off: usize, v: u32) void {
    std.mem.writeInt(u32, sector[off..][0..4], v, .little);
}

test "bind seeds the allocator state and rebind resets it" {
    reset();
    var m = mkFat32();
    defer _ = al.priv_alloc_state_release(&m);
    al.priv_alloc_state_bind(&m);
    try std.testing.expectEqual(@as(u32, 2), al.priv_alloc_hint_get(&m));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), al.priv_free_count_peek(&m));
    al.priv_alloc_hint_set(&m, 50);
    try std.testing.expectEqual(@as(u32, 50), al.priv_alloc_hint_get(&m));
    al.priv_alloc_state_bind(&m);
    try std.testing.expectEqual(@as(u32, 2), al.priv_alloc_hint_get(&m));
}

test "an unbound mount falls back to the defaults" {
    reset();
    var m = mkFat32();
    al.priv_alloc_hint_set(&m, 9);
    try std.testing.expectEqual(@as(u32, 2), al.priv_alloc_hint_get(&m));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), al.priv_free_count_peek(&m));
    al.priv_free_count_took(&m, 1);
    al.priv_free_count_gave(&m, 1);
    al.priv_alloc_hint_lower(&m, 1);
    try std.testing.expectEqual(@as(u32, 2), al.priv_alloc_hint_get(&m));
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_seed(&m));
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_flush(&m));
}

test "bind reuses a freed slot and a third mount finds none" {
    reset();
    var m1 = mkFat32();
    var m2 = mkFat32();
    var m3 = mkFat32();
    defer _ = al.priv_alloc_state_release(&m1);
    defer _ = al.priv_alloc_state_release(&m2);
    defer _ = al.priv_alloc_state_release(&m3);
    al.priv_alloc_state_bind(&m1);
    al.priv_alloc_state_bind(&m2);
    al.priv_alloc_hint_set(&m1, 10);
    al.priv_alloc_hint_set(&m2, 20);
    _ = al.priv_alloc_state_release(&m1);
    var m4 = mkFat32();
    defer _ = al.priv_alloc_state_release(&m4);
    al.priv_alloc_state_bind(&m4);
    al.priv_alloc_hint_set(&m4, 77);
    try std.testing.expectEqual(@as(u32, 77), al.priv_alloc_hint_get(&m4));
    try std.testing.expectEqual(@as(u32, 20), al.priv_alloc_hint_get(&m2));
    // No free slot left: binding a fifth mount is a silent no-op.
    var m5 = mkFat32();
    defer _ = al.priv_alloc_state_release(&m5);
    al.priv_alloc_state_bind(&m5);
    al.priv_alloc_hint_set(&m5, 9);
    try std.testing.expectEqual(@as(u32, 2), al.priv_alloc_hint_get(&m5));
}

test "the FAT sector cache serves the first copy and misses mirrors" {
    reset();
    var m = mkFat32();
    defer _ = al.priv_alloc_state_release(&m);
    al.priv_alloc_state_bind(&m);
    @memset(&disk[105], 0xAA);
    var buf: [bps]u8 = @splat(0);
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fat_sector_read(&m, 105, &buf));
    try std.testing.expectEqual(@as(u8, 0xAA), buf[0]);
    try std.testing.expectEqual(@as(u32, 1), read_calls);
    _ = al.priv_fat_sector_read(&m, 105, &buf);
    try std.testing.expectEqual(@as(u32, 1), read_calls);
    // Beyond the first FAT copy: every read goes to the backend.
    _ = al.priv_fat_sector_read(&m, 120, &buf);
    try std.testing.expectEqual(@as(u32, 2), read_calls);
    _ = al.priv_fat_sector_read(&m, 120, &buf);
    try std.testing.expectEqual(@as(u32, 3), read_calls);
    // Below the first FAT copy: the unsigned wrap rejects it too.
    _ = al.priv_fat_sector_read(&m, 50, &buf);
    try std.testing.expectEqual(@as(u32, 4), read_calls);
    // A mirror-write notice for a non-cacheable sector leaves the cache alone.
    @memset(&buf, 0xBB);
    al.priv_fat_sector_wrote(&m, &buf, 120);
    try std.testing.expectEqual(@as(u32, 4), read_calls);
    // A mirror-write notice inside the first copy updates the cache.
    al.priv_fat_sector_wrote(&m, &buf, 101);
    _ = al.priv_fat_sector_read(&m, 101, &buf);
    try std.testing.expectEqual(@as(u8, 0xBB), buf[0]);
    try std.testing.expectEqual(@as(u32, 4), read_calls);
}

test "release clears the cache only for the releasing mount" {
    reset();
    var m1 = mkFat32();
    var m2 = mkFat32();
    defer _ = al.priv_alloc_state_release(&m1);
    defer _ = al.priv_alloc_state_release(&m2);
    al.priv_alloc_state_bind(&m1);
    al.priv_alloc_state_bind(&m2);
    var buf: [bps]u8 = @splat(0);
    _ = al.priv_fat_sector_read(&m1, 100, &buf);
    try std.testing.expectEqual(@as(u32, 1), read_calls);
    // Releasing a mount that does not own the cache keeps it warm.
    _ = al.priv_alloc_state_release(&m2);
    _ = al.priv_fat_sector_read(&m1, 100, &buf);
    try std.testing.expectEqual(@as(u32, 1), read_calls);
    _ = al.priv_alloc_state_release(&m1);
    _ = al.priv_fat_sector_read(&m1, 100, &buf);
    try std.testing.expectEqual(@as(u32, 2), read_calls);
}

test "the exFAT bitmap LBA is cached per bound slot" {
    reset();
    var m = mkFat32();
    var m2 = mkFat32();
    defer _ = al.priv_alloc_state_release(&m);
    defer _ = al.priv_alloc_state_release(&m2);
    al.priv_alloc_state_bind(&m);
    var lba: u64 = 0;
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_exfat_bitmap_lba(&m, &lba));
    // Stub bitmap cluster 3 -> first_data_lba 20 + (3 - 2).
    try std.testing.expectEqual(@as(u64, 21), lba);
    try std.testing.expectEqual(@as(u32, 1), find_bitmap_calls);
    _ = al.priv_exfat_bitmap_lba(&m, &lba);
    try std.testing.expectEqual(@as(u32, 1), find_bitmap_calls);
    // An unbound mount has no cache to consult.
    _ = al.priv_exfat_bitmap_lba(&m2, &lba);
    try std.testing.expectEqual(@as(u32, 2), find_bitmap_calls);
    // Release drops the cache with the slot.
    _ = al.priv_alloc_state_release(&m);
    al.priv_alloc_state_bind(&m);
    _ = al.priv_exfat_bitmap_lba(&m, &lba);
    try std.testing.expectEqual(@as(u32, 3), find_bitmap_calls);
}

test "free counts clamp and the dirty flag follows" {
    reset();
    var m = mkFat32();
    defer _ = al.priv_alloc_state_release(&m);
    al.priv_alloc_state_bind(&m);
    // Unknown stays unknown; the take still marks the state dirty.
    al.priv_free_count_took(&m, 1);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), al.priv_free_count_peek(&m));
    al.priv_free_count_cache(&m, 200);
    try std.testing.expectEqual(@as(u32, 100), al.priv_free_count_peek(&m));
    al.priv_free_count_took(&m, 30);
    try std.testing.expectEqual(@as(u32, 70), al.priv_free_count_peek(&m));
    al.priv_free_count_took(&m, 100);
    try std.testing.expectEqual(@as(u32, 0), al.priv_free_count_peek(&m));
    al.priv_free_count_gave(&m, 25);
    try std.testing.expectEqual(@as(u32, 25), al.priv_free_count_peek(&m));
    al.priv_free_count_gave(&m, 500);
    try std.testing.expectEqual(@as(u32, 100), al.priv_free_count_peek(&m));
}

test "fsinfo seeds the hint and free count from a valid sector" {
    reset();
    var m = mkFat32();
    defer _ = al.priv_alloc_state_release(&m);
    put16(&disk[0], 48, 1);
    put32(&disk[1], 0, 0x41615252);
    put32(&disk[1], 484, 0x61417272);
    put32(&disk[1], 508, 0xAA550000);
    put32(&disk[1], 488, 60);
    put32(&disk[1], 492, 40);
    al.priv_alloc_state_bind(&m);
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_seed(&m));
    try std.testing.expectEqual(@as(u32, 60), al.priv_free_count_peek(&m));
    try std.testing.expectEqual(@as(u32, 40), al.priv_alloc_hint_get(&m));
}

test "fsinfo seed ignores an out-of-band hint or count" {
    reset();
    var m = mkFat32();
    defer _ = al.priv_alloc_state_release(&m);
    put16(&disk[0], 48, 1);
    put32(&disk[1], 0, 0x41615252);
    put32(&disk[1], 484, 0x61417272);
    put32(&disk[1], 508, 0xAA550000);
    put32(&disk[1], 488, 200);
    put32(&disk[1], 492, 500);
    al.priv_alloc_state_bind(&m);
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_seed(&m));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), al.priv_free_count_peek(&m));
    try std.testing.expectEqual(@as(u32, 2), al.priv_alloc_hint_get(&m));
}

test "fsinfo seed skips a non-FAT32 mount without reading" {
    reset();
    var m = mkFat32();
    defer _ = al.priv_alloc_state_release(&m);
    m.type = c.k_ra8_fs_type_fat16;
    al.priv_alloc_state_bind(&m);
    put16(&disk[0], 48, 1);
    put32(&disk[1], 0, 0x41615252);
    put32(&disk[1], 484, 0x61417272);
    put32(&disk[1], 508, 0xAA550000);
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_seed(&m));
    try std.testing.expectEqual(@as(u32, 0), read_calls);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), al.priv_free_count_peek(&m));
}

test "fsinfo seed skips bad signatures and an out-of-reserved sector" {
    reset();
    var m = mkFat32();
    defer _ = al.priv_alloc_state_release(&m);
    // Sector pointer inside the reserved region but with wrong signatures.
    put16(&disk[0], 48, 1);
    put32(&disk[1], 0, 0xDEAD_BEEF);
    al.priv_alloc_state_bind(&m);
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_seed(&m));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), al.priv_free_count_peek(&m));
    // A declared sector at or past the reserved region is treated as absent.
    reset();
    put16(&disk[0], 48, 10);
    put32(&disk[1], 0, 0x41615252);
    put32(&disk[1], 484, 0x61417272);
    put32(&disk[1], 508, 0xAA550000);
    al.priv_alloc_state_bind(&m);
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_seed(&m));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), al.priv_free_count_peek(&m));
    // An absent pointer reads only the boot sector.
    reset();
    al.priv_alloc_state_bind(&m);
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_seed(&m));
    try std.testing.expectEqual(@as(u32, 1), read_calls);
}

test "flush writes the counts back and clears the dirty flag" {
    reset();
    var m = mkFat32();
    defer _ = al.priv_alloc_state_release(&m);
    put16(&disk[0], 48, 1);
    put32(&disk[1], 0, 0x41615252);
    put32(&disk[1], 484, 0x61417272);
    put32(&disk[1], 508, 0xAA550000);
    put32(&disk[1], 488, 60);
    put32(&disk[1], 492, 40);
    al.priv_alloc_state_bind(&m);
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_seed(&m));
    // Not dirty yet: a flush is a no-op.
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_flush(&m));
    try std.testing.expectEqual(@as(u32, 0), write_calls);
    al.priv_free_count_took(&m, 10);
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_flush(&m));
    try std.testing.expectEqual(@as(u32, 1), write_calls);
    try std.testing.expectEqual(@as(u32, 50), std.mem.readInt(u32, disk[1][488..][0..4], .little));
    try std.testing.expectEqual(@as(u32, 40), std.mem.readInt(u32, disk[1][492..][0..4], .little));
    // The write cleared the flag: the next flush stays quiet.
    try std.testing.expectEqual(@as(u16, c.k_ra8_ok), al.priv_fsinfo_flush(&m));
    try std.testing.expectEqual(@as(u32, 1), write_calls);
}
