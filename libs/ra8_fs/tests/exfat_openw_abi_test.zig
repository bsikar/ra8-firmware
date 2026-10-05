//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT open-for-write (RA8FW-755).

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");
const ow = fs.exfat_openw;

const write_mode: c.ra8_fs_mode_t = c.k_ra8_fs_mode_write;
const append_mode: c.ra8_fs_mode_t = c.k_ra8_fs_mode_append;

/// An existing file: attributes, then a stream with cluster, length, valid length, flags.
fn existing(attr: u8, clus: u32, len: u64, valid: u64, flags: u8) void {
    fake.reset(c.k_ra8_fs_type_exfat);
    fake.mount_store.count_of_clusters = 30;
    fake.set_copies = true;
    fake.set_count = 3;
    fake.set_file[c.k_exfat_off_file_attr] = attr;
    fake.set_strm[c.k_exfat_strm_off_flags] = flags;
    std.mem.writeInt(u32, fake.set_strm[c.k_exfat_strm_off_clus..][0..4], clus, .little);
    std.mem.writeInt(u64, fake.set_strm[c.k_exfat_strm_off_dlen..][0..8], len, .little);
    std.mem.writeInt(u64, fake.set_strm[c.k_exfat_off_strm_valid..][0..8], valid, .little);
}

fn open(mode: c.ra8_fs_mode_t, out: *?*ow.File) u16 {
    return ow.priv_exfat_open_write(&fake.mount_store, "/dir/f.bin", mode, out);
}

test "write mode frees the clusters and truncates" {
    existing(0, 10, 8192, 8192, 0);
    var f: ?*ow.File = null;
    try std.testing.expectEqual(ow.ok, open(write_mode, &f));
    const h = f.?;
    try std.testing.expectEqual(@as(u32, 1), fake.free_calls);
    try std.testing.expectEqual(@as(u32, 1), fake.flush_calls);
    try std.testing.expectEqual(@as(u32, 0), h.first_cluster);
    try std.testing.expectEqual(@as(u64, 0), @as(u64, h.size_bytes));
    try std.testing.expectEqual(@as(u32, 3), @as(u32, h.entry_set_count));
    try std.testing.expectEqual(@as(u32, 2), @as(u32, h.entry_set_index));
    try std.testing.expectEqual(@as(u32, 1), @as(u32, h.in_use));
}

test "append on a contiguous file sizes the run and clamps valid length" {
    existing(0, 10, 9000, 10000, c.k_exfat_secflag_no_fat);
    var f: ?*ow.File = null;
    try std.testing.expectEqual(ow.ok, open(append_mode, &f));
    const h = f.?;
    try std.testing.expectEqual(@as(u32, 3), h.alloc_clusters);
    try std.testing.expectEqual(@as(u32, 12), h.tail_cluster);
    try std.testing.expectEqual(@as(u64, 9000), @as(u64, h.valid_bytes));
    try std.testing.expectEqual(@as(u64, 9000), @as(u64, h.offset));
    try std.testing.expectEqual(@as(u32, 10), h.cur_cluster);
    try std.testing.expectEqual(@as(u32, 0), fake.free_calls);
}

test "append on a FAT-chained file walks the chain; a bad link is refused" {
    existing(0, 10, 5000, 5000, 0);
    fake.fat[10] = 11;
    fake.fat[11] = 0x0FFF_FFFF;
    var f: ?*ow.File = null;
    try std.testing.expectEqual(ow.ok, open(append_mode, &f));
    try std.testing.expectEqual(@as(u32, 2), f.?.alloc_clusters);
    try std.testing.expectEqual(@as(u32, 11), f.?.tail_cluster);

    existing(0, 10, 5000, 5000, 0);
    fake.fat[10] = 1;
    f = null;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_protocol_error), open(append_mode, &f));
    try std.testing.expect(f == null);
    try std.testing.expectEqual(@as(u32, 0), @as(u32, fake.file_pool[1].in_use));
}

test "directories, read-only files and unwritable sets are refused" {
    var f: ?*ow.File = null;
    existing(c.k_exfat_attr_directory, 10, 1, 1, 0);
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_invalid_arg), open(write_mode, &f));
    existing(c.k_exfat_attr_read_only, 10, 1, 1, 0);
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_access_denied), open(append_mode, &f));
    existing(0, 10, 1, 1, 0);
    fake.set_count = 8;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_not_supported), open(write_mode, &f));
    existing(0, 10, 1, 1, 0);
    fake.set_count = 2;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_protocol_error), open(write_mode, &f));
    try std.testing.expect(f == null);
    try std.testing.expectEqual(@as(u32, 2), fake.slots_left);
}

test "a missing leaf is created as an empty set" {
    fake.reset(c.k_ra8_fs_type_exfat);
    fake.find_set_err = c.k_ra8_err_not_found;
    fake.link_count = 4;
    var f: ?*ow.File = null;
    try std.testing.expectEqual(ow.ok, open(write_mode, &f));
    try std.testing.expectEqual(@as(u32, 4), @as(u32, f.?.entry_set_count));
    try std.testing.expectEqual(@as(u32, 6), @as(u32, f.?.entry_set_index));
    try std.testing.expectEqual(@as(u32, 0), f.?.first_cluster);

    fake.reset(c.k_ra8_fs_type_exfat);
    fake.find_set_err = c.k_ra8_err_not_found;
    fake.link_err = c.k_ra8_err_hw_error;
    f = null;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_hw_error), open(write_mode, &f));
    try std.testing.expectEqual(@as(u32, 0), @as(u32, fake.file_pool[1].in_use));

    fake.reset(c.k_ra8_fs_type_exfat);
    fake.find_set_err = c.k_ra8_err_not_found;
    fake.slots_left = 0;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_no_mem), open(write_mode, &f));
}

test "path, name and lookup errors pass through" {
    var f: ?*ow.File = null;
    fake.reset(c.k_ra8_fs_type_exfat);
    fake.parent_err = c.k_ra8_err_not_found;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_not_found), open(write_mode, &f));
    fake.reset(c.k_ra8_fs_type_exfat);
    fake.name_err = c.k_ra8_err_no_mem;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_invalid_arg), open(write_mode, &f));
    fake.reset(c.k_ra8_fs_type_exfat);
    fake.name_units = 0;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_invalid_arg), open(write_mode, &f));
    fake.reset(c.k_ra8_fs_type_exfat);
    fake.find_set_err = c.k_ra8_err_hw_error;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_hw_error), open(write_mode, &f));
    try std.testing.expect(f == null);
}
