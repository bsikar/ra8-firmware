//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the shared FAT/exFAT module state: the scratch and zero
//! sectors, and the mount/file slot allocators moved out of
//! `ra8_fs_fat_mount.c` (RA8FW-1143).

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const st = fs.fat_state;

test "g_fs_scratch is full sector size and starts zeroed" {
    try std.testing.expectEqual(@as(usize, c.k_ra8_fs_sector_max), st.g_fs_scratch.len);
    for (st.g_fs_scratch) |b| try std.testing.expectEqual(@as(u8, 0), b);
}

test "k_zero_sector is a full sector of zeros" {
    try std.testing.expectEqual(@as(usize, c.k_ra8_fs_sector_max), st.k_zero_sector.len);
    for (st.k_zero_sector) |b| try std.testing.expectEqual(@as(u8, 0), b);
}

test "file allocator hands out distinct slots, then exhausts" {
    st.filesReset();
    var first: usize = c.k_ra8_fs_max_files;
    var taken: usize = 0;
    while (taken < c.k_ra8_fs_max_files) : (taken += 1) {
        const f = st.priv_alloc_file_slot();
        try std.testing.expect(f != null);
        const idx = (@intFromPtr(f.?) - @intFromPtr(&st.s_files)) / @sizeOf(c.ra8_fs_file_t);
        if (taken == 0) first = idx;
        try std.testing.expectEqual(first + taken, idx);
        f.?.in_use = 1;
    }
    try std.testing.expect(st.priv_alloc_file_slot() == null);
    st.filesReset();
    try std.testing.expect(st.priv_alloc_file_slot() != null);
}

test "filesFill stands in for a full table" {
    st.filesReset();
    st.filesFill();
    try std.testing.expect(st.priv_alloc_file_slot() == null);
}

test "mount allocator mirrors the file allocator" {
    var i: usize = 0;
    while (i < c.k_ra8_fs_max_mounts) : (i += 1) {
        const m = st.internal_alloc_mount_slot();
        try std.testing.expect(m != null);
        m.?.in_use = 1;
    }
    try std.testing.expect(st.internal_alloc_mount_slot() == null);
}
