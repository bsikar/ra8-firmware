//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT open-for-write (RA8FW-755). Real entry sets staged on the walker
//! fake: the root carries a "dir" directory set, its cluster carries f.bin.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");
const st = fs.fat_state;
const ow = fs.exfat_openw;

const write_mode: c.ra8_fs_mode_t = c.k_ra8_fs_mode_write;
const append_mode: c.ra8_fs_mode_t = c.k_ra8_fs_mode_append;

const entry_bytes: usize = 32;
const entry_file: u8 = @intCast(c.k_exfat_entry_file);
const entry_stream: u8 = @intCast(c.k_exfat_entry_stream);
const entry_name: u8 = 0xC1;
const off_file_attr: usize = @intCast(c.k_exfat_off_file_attr);
const off_file_secnt: usize = @intCast(c.k_exfat_off_file_secnt);
const strm_off_flags: usize = @intCast(c.k_exfat_strm_off_flags);
const strm_off_nlen: usize = @intCast(c.k_exfat_strm_off_nlen);
const off_strm_valid: usize = @intCast(c.k_exfat_off_strm_valid);
const strm_off_clus: usize = @intCast(c.k_exfat_strm_off_clus);
const strm_off_dlen: usize = @intCast(c.k_exfat_strm_off_dlen);
const name_off: usize = @intCast(c.k_exfat_name_off);
const no_fat: u8 = @intCast(c.k_exfat_secflag_no_fat);
const eoc: u32 = 0xFFFF_FFFF;

/// The dir cluster hosting f.bin: cluster 3 lands on LBA 2, which the fake
/// serves from disk[1] (first_data_lba 1, one sector per cluster).
const dir_clus: u32 = 3;
const fbin_slot: usize = 2;

fn wr32(p: []u8, at: usize, v: u32) void {
    p[at] = @truncate(v);
    p[at + 1] = @truncate(v >> 8);
    p[at + 2] = @truncate(v >> 16);
    p[at + 3] = @truncate(v >> 24);
}

fn wr64(p: []u8, at: usize, v: u64) void {
    wr32(p, at, @truncate(v));
    wr32(p, at + 4, @truncate(v >> 32));
}

fn rd32(p: []const u8, at: usize) u32 {
    return @as(u32, p[at]) | (@as(u32, p[at + 1]) << 8) | (@as(u32, p[at + 2]) << 16) | (@as(u32, p[at + 3]) << 24);
}

fn layName(p: []u8, base: usize, name: []const u8) void {
    p[base] = entry_name;
    for (name, 0..) |ch, i| p[base + name_off + i * 2] = ch;
    p[base + name_off + name.len * 2] = 0xFF;
    p[base + name_off + name.len * 2 + 1] = 0xFF;
}

/// Lay one real set (File + Stream + secnt - 1 secondaries) at `slot` of a
/// flat 512-byte sector image.
fn laySet(p: []u8, slot: usize, name: []const u8, attr: u8, first: u32, dlen: u64, flags: u8, secnt: u8) void {
    const base = slot * entry_bytes;
    p[base] = entry_file;
    p[base + off_file_secnt] = secnt;
    p[base + off_file_attr] = attr;
    const s = base + entry_bytes;
    p[s] = entry_stream;
    p[s + strm_off_flags] = flags;
    p[s + strm_off_nlen] = @intCast(name.len);
    wr32(p, s + strm_off_clus, first);
    wr32(p, s + strm_off_dlen, @truncate(dlen));
    wr32(p, s + strm_off_dlen + 4, @truncate(dlen >> 32));
    wr64(p, s + off_strm_valid, dlen);
    var i: usize = 1;
    while (i < secnt) : (i += 1) {
        layName(p, base + i * entry_bytes + entry_bytes, name);
    }
    // The set's checksum covers the whole set; stamp it real.
    _ = c.priv_exfat_set_checksum(@ptrCast(&p[base]), @intCast(secnt * entry_bytes + entry_bytes));
}

/// An existing file: the root carries a real "dir" directory set whose
/// cluster holds f.bin's set with the given attributes and stream fields.
fn existing(attr: u8, clus: u32, len: u64, valid: u64, flags: u8) void {
    existingCount(attr, clus, len, valid, flags, 2);
}

fn existingCount(attr: u8, clus: u32, len: u64, valid: u64, flags: u8, secnt: u8) void {
    _ = valid;
    fake.reset(c.k_ra8_fs_type_exfat);
    fake.mount_store.count_of_clusters = 30;
    // Root: reset stages the two system entries; "dir" follows at slot 2.
    laySet(std.mem.sliceAsBytes(fake.dir[2..5]), 0, "dir", @intCast(c.k_exfat_attr_directory), dir_clus, 512, no_fat, 2);
    // The dir cluster: two name entries the scan skips, then f.bin's set.
    const d: []u8 = &fake.disk[1];
    d[0] = entry_name;
    d[32] = entry_name;
    laySet(d, fbin_slot, "f.bin", attr, clus, len, flags, secnt);
    // A flags==0 stream walks the FAT: end the chain at its first cluster.
    if (flags == 0) fake.fat[clus] = eoc;
}

fn open(mode: c.ra8_fs_mode_t, out: *?*ow.File) u16 {
    return ow.priv_exfat_open_write(&fake.mount_store, "/dir/f.bin", mode, out);
}

fn openAt(path: [*:0]const u8, mode: c.ra8_fs_mode_t, out: *?*ow.File) u16 {
    return ow.priv_exfat_open_write(&fake.mount_store, path, mode, out);
}

test "write mode frees the clusters and truncates" {
    existing(0, 10, 8192, 8192, 0);
    var f: ?*ow.File = null;
    try std.testing.expectEqual(ow.ok, open(write_mode, &f));
    const h = f.?;
    try std.testing.expectEqual(@as(u32, 0), h.first_cluster);
    try std.testing.expectEqual(@as(u64, 0), @as(u64, h.size_bytes));
    try std.testing.expectEqual(@as(u32, 3), @as(u32, h.entry_set_count));
    try std.testing.expectEqual(@as(u32, 2), @as(u32, h.entry_set_index));
    try std.testing.expectEqual(@as(u32, 1), @as(u32, h.in_use));
}

test "append on a contiguous file sizes the run and clamps valid length" {
    existing(0, 10, 9000, 10000, no_fat);
    var f: ?*ow.File = null;
    try std.testing.expectEqual(ow.ok, open(append_mode, &f));
    const h = f.?;
    try std.testing.expectEqual(@as(u32, 18), h.alloc_clusters); // ceil(9000 / 512)
    try std.testing.expectEqual(@as(u32, 27), h.tail_cluster);
    try std.testing.expectEqual(@as(u64, 9000), @as(u64, h.valid_bytes));
    try std.testing.expectEqual(@as(u64, 9000), @as(u64, h.offset));
    try std.testing.expectEqual(@as(u32, 10), h.cur_cluster);
}

test "append on a FAT-chained file walks the chain; a bad link is refused" {
    existing(0, 10, 5000, 5000, 0);
    fake.fat[10] = 11;
    fake.fat[11] = eoc;
    var f: ?*ow.File = null;
    try std.testing.expectEqual(ow.ok, open(append_mode, &f));
    try std.testing.expectEqual(@as(u32, 2), f.?.alloc_clusters);
    try std.testing.expectEqual(@as(u32, 11), f.?.tail_cluster);

    existing(0, 10, 5000, 5000, 0);
    fake.fat[10] = 1;
    f = null;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_protocol_error), open(append_mode, &f));
    try std.testing.expect(f == null);
    try std.testing.expectEqual(@as(u32, 0), @as(u32, st.s_files[1].in_use));
}

test "directories, read-only files and unwritable sets are refused" {
    var f: ?*ow.File = null;
    existing(@intCast(c.k_exfat_attr_directory), 10, 1, 1, 0);
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_invalid_arg), open(write_mode, &f));
    existing(@intCast(c.k_exfat_attr_read_only), 10, 1, 1, 0);
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_access_denied), open(append_mode, &f));
    existingCount(0, 10, 1, 1, 0, 7);
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_not_supported), open(write_mode, &f));
    existingCount(0, 10, 1, 1, 0, 1);
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_protocol_error), open(write_mode, &f));
    try std.testing.expect(f == null);
}

test "a missing leaf is created as an empty set" {
    fake.reset(c.k_ra8_fs_type_exfat);
    fake.mount_store.count_of_clusters = 30;
    laySet(std.mem.sliceAsBytes(fake.dir[2..5]), 0, "dir", @intCast(c.k_exfat_attr_directory), dir_clus, 512, no_fat, 2);
    var f: ?*ow.File = null;
    try std.testing.expectEqual(ow.ok, open(write_mode, &f));
    try std.testing.expectEqual(@as(u32, 3), @as(u32, f.?.entry_set_count));
    try std.testing.expectEqual(@as(u32, 0), f.?.first_cluster);

    fake.reset(c.k_ra8_fs_type_exfat);
    fake.mount_store.count_of_clusters = 30;
    laySet(std.mem.sliceAsBytes(fake.dir[2..5]), 0, "dir", @intCast(c.k_exfat_attr_directory), dir_clus, 512, no_fat, 2);
    st.filesFill();
    f = null;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_no_mem), open(write_mode, &f));
}

test "path, name and lookup errors pass through" {
    var f: ?*ow.File = null;
    fake.reset(c.k_ra8_fs_type_exfat);
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_not_found), openAt("/nope/f.bin", write_mode, &f));
    fake.reset(c.k_ra8_fs_type_exfat);
    var long_name: [67]u8 = @splat('a');
    long_name[66] = 0;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_invalid_arg), openAt(long_name[0..66 :0], write_mode, &f));
    fake.reset(c.k_ra8_fs_type_exfat);
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_invalid_arg), openAt("", write_mode, &f));
}
