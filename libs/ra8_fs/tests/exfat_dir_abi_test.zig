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
const err_io: u16 = c.k_ra8_err_io;
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

const bps: u32 = 512;
const spc: u32 = 2;
const root_cluster: u32 = 2;
const data_start_lba: u64 = 64;

// ---------------------------------------------------------------------------
// Fake disk + scripted directory tree
// ---------------------------------------------------------------------------

var disk: [128][sector_max]u8 = undefined;
var m0: c.ra8_fs_mount_t = undefined;

const Slot = struct {
    name: []const u8,
    attr: u8,
    first: u32,
    live: bool,
};
const DirImage = struct {
    cluster: u32,
    slots: [6]Slot = undefined,
    n: usize = 0,
    entries: [8][entry_bytes]u8 = undefined,
    entry_n: usize = 0,
};
var dirs: [8]DirImage = undefined;
var dir_n: usize = 0;

fn imageOf(cluster: u32) ?*DirImage {
    for (dirs[0..dir_n]) |*d| {
        if (d.cluster == cluster) return d;
    }
    return null;
}

fn fillEntry(e: []u8, typ: u8, live: bool, attr: u8, first: u32) void {
    @memset(e, 0);
    e[0] = if (live) typ | inuse_bit else typ & ~inuse_bit;
    if (typ == entry_file) e[off_file_attr] = attr;
    if (typ == entry_stream) std.mem.writeInt(u32, e[strm_off_clus..][0..4], first, .little);
}

fn addSlot(cluster: u32, name: []const u8, attr: u8, first: u32, live: bool) void {
    const d = imageOf(cluster).?;
    d.slots[d.n] = .{ .name = name, .attr = attr, .first = first, .live = live };
    d.n += 1;
    // One File + one Stream raw entry pair, so the cursor walk sees occupants.
    if (d.entry_n + 2 <= d.entries.len) {
        fillEntry(&d.entries[d.entry_n], entry_file, live, attr, first);
        fillEntry(&d.entries[d.entry_n + 1], entry_stream, live, attr, first);
        d.entry_n += 2;
    }
}

fn addImage(cluster: u32) void {
    dirs[dir_n] = .{ .cluster = cluster };
    dir_n += 1;
}

fn resetFs() void {
    disk = @splat(@splat(0xAA));
    m0 = std.mem.zeroes(c.ra8_fs_mount_t);
    m0.sectors_per_cluster = spc;
    m0.bytes_per_sector = bps;
    dir_n = 0;
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
    addImage(root_cluster);
}

fn stdImage(cluster: u32) void {
    addImage(cluster);
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

fn rd32(p: []const u8, off: usize) u32 {
    return std.mem.readInt(u32, p[off..][0..4], .little);
}

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

export fn priv_strlen(s: [*c]const u8) callconv(.c) u32 {
    var n: u32 = 0;
    while (s[n] != 0) n += 1;
    return n;
}

export fn priv_bps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32 {
    _ = m;
    return bps;
}

export fn priv_cluster_to_lba(m: [*c]const c.ra8_fs_mount_t, cluster: u32) callconv(.c) u64 {
    _ = m;
    return data_start_lba + @as(u64, cluster - first_data) * spc;
}

export fn priv_read_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.c) u16 {
    _ = m;
    if (lba >= disk.len) return err_io;
    @memcpy(buf[0..sector_max], &disk[@intCast(lba)]);
    return ok;
}

export fn priv_write_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]const u8) callconv(.c) u16 {
    _ = m;
    if (lba >= disk.len) return err_io;
    @memcpy(&disk[@intCast(lba)], buf[0..sector_max]);
    return ok;
}

export fn priv_exfat_name_to_units(m: [*c]const c.ra8_fs_mount_t, name: [*c]const u8, units: [*c]u16, nunits: [*c]u32) callconv(.c) u16 {
    _ = m;
    var n: u32 = 0;
    while (name[n] != 0) {
        if (n >= name_cap) return err_no_mem;
        units[n] = name[n];
        n += 1;
    }
    nunits[0] = n;
    return ok;
}

export fn priv_exfat_dir_root(m: [*c]const c.ra8_fs_mount_t, out: [*c]c.exfat_dir_t) callconv(.c) void {
    _ = m;
    out[0] = .{ .cluster = root_cluster, .contig_end = 0, .self_cluster = 0, .self_index = 0 };
}

export fn priv_exfat_dir_from_set(m: [*c]const c.ra8_fs_mount_t, strm: [*c]const u8, out: [*c]c.exfat_dir_t) callconv(.c) void {
    _ = m;
    out[0].cluster = rd32(strm[0..entry_bytes], strm_off_clus);
    out[0].contig_end = 0;
}

export fn priv_exfat_find_set(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, path: [*c]const u8, pos: [*c]c.exfat_setpos_t, pos_cap: u32, out_count: [*c]u32, out_file: [*c]u8, out_strm: [*c]u8) callconv(.c) u16 {
    _ = m;
    _ = pos_cap;
    if (find_set_err != 0) return find_set_err;
    const d = imageOf(dir[0].cluster) orelse return err_not_found;
    var i: usize = 0;
    while (i < d.n) : (i += 1) {
        const s = d.slots[i];
        if (!std.mem.eql(u8, s.name, std.mem.span(@as([*:0]const u8, @ptrCast(path))))) continue;
        pos[0] = .{ .cluster = dir[0].cluster, .index = @intCast(i * 2) };
        out_count[0] = 1;
        fillEntry(out_file[0..entry_bytes], entry_file, s.live, s.attr, s.first);
        fillEntry(out_strm[0..entry_bytes], entry_stream, s.live, s.attr, s.first);
        return ok;
    }
    return err_not_found;
}

export fn priv_exfat_find(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, path: [*c]const u8, out_strm: [*c]u8, out_attr: [*c]u8) callconv(.c) u16 {
    if (find_err != 0) return find_err;
    const d = imageOf(dir[0].cluster) orelse return err_not_found;
    var i: usize = 0;
    while (i < d.n) : (i += 1) {
        const s = d.slots[i];
        if (!std.mem.eql(u8, s.name, std.mem.span(@as([*:0]const u8, @ptrCast(path))))) continue;
        fillEntry(out_strm[0..entry_bytes], entry_stream, s.live, s.attr, s.first);
        out_attr[0] = s.attr;
        return ok;
    }
    return err_not_found;
}

export fn priv_exfat_cursor_init(dir: [*c]const c.exfat_dir_t, out: [*c]c.exfat_cursor_t) callconv(.c) void {
    out[0] = .{ .cluster = dir[0].cluster, .entry_in_cluster = 0, .scanned = 0, .contig_end = dir[0].contig_end };
}

export fn priv_exfat_next_entry(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.exfat_cursor_t, out: [*c]u8) callconv(.c) u16 {
    _ = m;
    if (next_err != 0) return next_err;
    const d = imageOf(cur[0].cluster) orelse return err_not_found;
    const idx: usize = @intCast(cur[0].entry_in_cluster);
    if (idx < d.entry_n) {
        @memcpy(out[0..entry_bytes], &d.entries[idx]);
        cur[0].entry_in_cluster += 1;
        cur[0].scanned += 1;
        return ok;
    }
    if (idx < d.entries.len) {
        @memset(out[0..entry_bytes], 0); // end-of-directory marker
        cur[0].entry_in_cluster += 1;
        cur[0].scanned += 1;
        return ok;
    }
    return err_not_found;
}

export fn priv_exfat_find_bitmap(m: [*c]const c.ra8_fs_mount_t, out_clus: [*c]u32, out_len: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_clus[0] = 1;
    out_len[0] = 8;
    return ok;
}

export fn priv_exfat_bitmap_scan(m: [*c]const c.ra8_fs_mount_t, bmp_lba: u64, count: u32, out_clus: [*c]u32) callconv(.c) u16 {
    _ = m;
    _ = bmp_lba;
    _ = count;
    out_clus[0] = scan_free;
    return ok;
}

export fn priv_exfat_bitmap_mark(m: [*c]const c.ra8_fs_mount_t, bmp_lba: u64, clus: u32, count: u32) callconv(.c) u16 {
    _ = m;
    _ = count;
    mark_count += 1;
    mark_lba = bmp_lba;
    mark_clus = clus;
    return ok;
}

export fn priv_exfat_bitmap_clear(m: [*c]const c.ra8_fs_mount_t, bmp_lba: u64, clus: u32, count: u32) callconv(.c) u16 {
    _ = m;
    _ = count;
    clear_count += 1;
    clear_lba = bmp_lba;
    clear_clus = clus;
    return ok;
}

export fn priv_alloc_hint_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32) callconv(.c) void {
    _ = m;
    hint_count += 1;
    hint_clus = cluster;
}

export fn priv_exfat_write_dir_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32, idx: u32, set: [*c]const u8, bytes: u32) callconv(.c) u16 {
    _ = m;
    if (wdir_err != 0) return wdir_err;
    if (wdir_count < 8) {
        wdir_cluster[wdir_count] = cluster;
        wdir_index[wdir_count] = idx;
        wdir_entry0[wdir_count] = set[0];
        wdir_bytes[wdir_count] = bytes;
        @memcpy(&wdir_set[wdir_count], set[0..max_set_bytes]);
        wdir_clus_field[wdir_count] = rd32(set[0..max_set_bytes], entry_bytes + strm_off_clus);
    }
    wdir_count += 1;
    return ok;
}

export fn priv_exfat_file_stamp_create(file_entry: [*c]u8) callconv(.c) void {
    file_entry[7] = 0xAB;
}

export fn priv_exfat_name_hash(name: [*c]const u16, nlen: u32) callconv(.c) u16 {
    return @intCast(nlen * 0x11 + 1);
}

export fn priv_exfat_set_checksum(set: [*c]const u8, bytes: u32) callconv(.c) u16 {
    _ = set;
    _ = bytes;
    return 0xBEEF;
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

export fn priv_exfat_drop_set(m: [*c]const c.ra8_fs_mount_t, pos: [*c]const c.exfat_setpos_t, count: u32) callconv(.c) u16 {
    _ = m;
    _ = pos;
    drop_count += 1;
    drop_n = count;
    return ok;
}

export fn priv_exfat_free_clusters(m: [*c]const c.ra8_fs_mount_t, strm: [*c]const u8) callconv(.c) u16 {
    _ = m;
    free_count += 1;
    free_clus = rd32(strm[0..entry_bytes], strm_off_clus);
    return ok;
}

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
    const path = std.fmt.bufPrintZ(&path_buf, "/{s}/y", .{long_name}) catch unreachable;
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
    space_clus = root_cluster;
    space_idx = 2;
    scan_free = 9;
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_mkdir(&m0, "/newdir"));
    try std.testing.expectEqual(@as(u32, 1), wdir_count);
    try std.testing.expectEqual(root_cluster, wdir_cluster[0]);
    try std.testing.expectEqual(@as(u32, 2), wdir_index[0]);
    try std.testing.expectEqual(entry_file | inuse_bit, wdir_entry0[0]);
    try std.testing.expectEqual(@as(u32, 9), wdir_clus_field[0]);
    try std.testing.expectEqual(@as(u32, 1), mark_count);
    try std.testing.expectEqual(@as(u32, 9), mark_clus);
    try std.testing.expectEqual(@as(u32, 1), hint_count);
    try std.testing.expectEqual(@as(u32, 10), hint_clus);
}

test "mkdir set bytes: attribute, secondary count, name units, checksum" {
    resetFs();
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_mkdir(&m0, "/sub"));
    const set = &wdir_set[0];
    try std.testing.expectEqual(entry_file, set[0]);
    try std.testing.expectEqual(@as(u8, 2), set[off_file_secnt]); // File + Stream + 1 Name
    try std.testing.expectEqual(@as(u16, attr_directory), std.mem.readInt(u16, set[off_file_attr..][0..2], .little));
    try std.testing.expectEqual(@as(u8, 0xAB), set[7]); // the create stamp the fake writes
    const strm = set[entry_bytes..];
    try std.testing.expectEqual(entry_stream, strm[0]);
    try std.testing.expectEqual(@as(u8, 0x03), strm[strm_off_flags]); // alloc + NoFatChain
    try std.testing.expectEqual(@as(u8, 3), strm[strm_off_nlen]);
    try std.testing.expectEqual(@as(u16, 3 * 0x11 + 1), std.mem.readInt(u16, strm[off_strm_hash..][0..2], .little));
    try std.testing.expectEqual(@as(u32, bps * spc), rd32(strm, off_strm_valid));
    try std.testing.expectEqual(@as(u32, bps * spc), rd32(strm, strm_off_dlen));
    const name_e = set[2 * entry_bytes ..];
    try std.testing.expectEqual(entry_name, name_e[0]);
    try std.testing.expectEqual(@as(u16, 's'), std.mem.readInt(u16, name_e[name_off..][0..2], .little));
    try std.testing.expectEqual(@as(u16, 'u'), std.mem.readInt(u16, name_e[name_off + 2 ..][0..2], .little));
    try std.testing.expectEqual(@as(u16, 0xBEEF), std.mem.readInt(u16, set[off_file_csum..][0..2], .little));
}

test "mkdir refuses the root, an over-long name, and an existing name" {
    resetFs();
    addSlot(root_cluster, "dup", 0x20, 7, true);
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_mkdir(&m0, "/"));
    const long_name: [65]u8 = @splat('y');
    var path_buf: [80]u8 = undefined;
    const path = std.fmt.bufPrintZ(&path_buf, "/{s}", .{long_name}) catch unreachable;
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_mkdir(&m0, path));
    try std.testing.expectEqual(err_exists, dir_abi.priv_exfat_mkdir(&m0, "/dup"));
}

test "mkdir rolls the allocated cluster back when the set write fails" {
    resetFs();
    wdir_err = err_io;
    try std.testing.expectEqual(err_io, dir_abi.priv_exfat_mkdir(&m0, "/late"));
    try std.testing.expectEqual(@as(u32, 1), clear_count);
    try std.testing.expectEqual(@as(u32, 9), clear_clus);
}

test "rmdir retires the set and frees the cluster of an empty directory" {
    resetFs();
    stdImage(5);
    stdImage(6);
    addSlot(root_cluster, "a", attr_directory, 5, true);
    addSlot(5, "empty", attr_directory, 6, true);
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_rmdir(&m0, "/a/empty"));
    try std.testing.expectEqual(@as(u32, 1), drop_count);
    try std.testing.expectEqual(@as(u32, 1), drop_n);
    try std.testing.expectEqual(@as(u32, 1), free_count);
    try std.testing.expectEqual(@as(u32, 6), free_clus);
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
    stdImage(5);
    stdImage(8);
    addSlot(root_cluster, "a", attr_directory, 5, true);
    addSlot(5, "tidy", attr_directory, 8, true);
    addSlot(8, "gone.txt", 0x20, 12, false);
    try std.testing.expectEqual(ok, dir_abi.priv_exfat_rmdir(&m0, "/a/tidy"));
    try std.testing.expectEqual(@as(u32, 1), free_count);
}

test "rmdir refuses the root, a file, and propagates lookup failures" {
    resetFs();
    addSlot(root_cluster, "f.txt", 0x20, 12, true);
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_rmdir(&m0, "/"));
    try std.testing.expectEqual(err_invalid_arg, dir_abi.priv_exfat_rmdir(&m0, "/f.txt"));
    find_set_err = err_io;
    try std.testing.expectEqual(err_io, dir_abi.priv_exfat_rmdir(&m0, "/gone"));
    find_set_err = 0;
}
