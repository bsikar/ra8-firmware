//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_fs fmt (mkfs) on FAT12/16/32 (RA8FW-1121), against the walker fakes.

const std = @import("std");
const fs = @import("ra8_fs");
const fmt = fs.fmt;
const c = fs.c;
comptime {
    _ = @import("fs_walker_fake.zig");
}

const fat12: u8 = c.k_ra8_fs_type_fat12;
const fat16: u8 = c.k_ra8_fs_type_fat16;
const fat32: u8 = c.k_ra8_fs_type_fat32;

fn geomFor(vol_type: u8, total_sectors: u64, bps: u32, root_entries: u32, root_sectors: u32) c.ra8_fs_fmt_geom_t {
    var g = std.mem.zeroes(c.ra8_fs_fmt_geom_t);
    g.type = vol_type;
    g.total_sectors = total_sectors;
    g.bytes_per_sector = bps;
    g.root_entries = root_entries;
    g.root_sectors = root_sectors;
    g.reserved_sectors = fmt.priv_fmt_reserved_for(vol_type);
    return g;
}

test "reserved-sector count follows the type" {
    try std.testing.expectEqual(@as(u32, 32), fmt.priv_fmt_reserved_for(fat32));
    try std.testing.expectEqual(@as(u32, 1), fmt.priv_fmt_reserved_for(fat16));
    try std.testing.expectEqual(@as(u32, 1), fmt.priv_fmt_reserved_for(fat12));
}

test "spc hint validity" {
    try std.testing.expect(fmt.priv_fmt_spc_valid(0));
    try std.testing.expect(fmt.priv_fmt_spc_valid(1));
    try std.testing.expect(fmt.priv_fmt_spc_valid(2));
    try std.testing.expect(fmt.priv_fmt_spc_valid(64));
    try std.testing.expect(!fmt.priv_fmt_spc_valid(3));
    try std.testing.expect(!fmt.priv_fmt_spc_valid(128));
}

test "label field pads, or writes the NO NAME sentinel" {
    var buf: [11]u8 = undefined;
    fmt.priv_fmt_label_field(&buf, "BRIGHTON");
    try std.testing.expectEqualStrings("BRIGHTON   ", &buf);
    fmt.priv_fmt_label_field(&buf, "");
    try std.testing.expectEqualStrings("NO NAME    ", &buf);
    fmt.priv_fmt_label_field(&buf, null);
    try std.testing.expectEqualStrings("NO NAME    ", &buf);
    fmt.priv_fmt_label_field(&buf, "THIS LABEL IS TOO LONG");
    try std.testing.expectEqualStrings("THIS LABEL ", &buf);
}

test "FAT16 auto geometry sweeps from the minimum cluster" {
    // 8 MB card, 512-byte sectors, 512-entry root: spc=1 fits the FAT16 band.
    var g = geomFor(fat16, 16384, 512, 512, 32);
    try std.testing.expectEqual(fmt.ok, fmt.priv_fmt_choose_geometry(&g, 0));
    try std.testing.expectEqual(@as(u32, 1), g.sectors_per_cluster);
    try std.testing.expectEqual(@as(u32, 64), g.fat_size_sectors);
    try std.testing.expectEqual(@as(u32, 16223), g.count_of_clusters);
}

test "FAT16 with a pinned cluster honors the hint" {
    var g = geomFor(fat16, 16384, 512, 512, 32);
    try std.testing.expectEqual(fmt.ok, fmt.priv_fmt_choose_geometry(&g, 2));
    try std.testing.expectEqual(@as(u32, 2), g.sectors_per_cluster);
    try std.testing.expectEqual(@as(u32, 32), g.fat_size_sectors);
    try std.testing.expectEqual(@as(u32, 8143), g.count_of_clusters);
}

test "FAT32 starts the sweep at the MS-table cluster" {
    // 128 MB card: table tier says 512-byte clusters, so spc=1.
    var g = geomFor(fat32, 262144, 512, 0, 0);
    try std.testing.expectEqual(fmt.ok, fmt.priv_fmt_choose_geometry(&g, 0));
    try std.testing.expectEqual(@as(u32, 1), g.sectors_per_cluster);
    try std.testing.expectEqual(@as(u32, 2017), g.fat_size_sectors);
    try std.testing.expectEqual(@as(u32, 258078), g.count_of_clusters);
}

test "FAT32 8 GB card lands on 4 KB clusters" {
    var g = geomFor(fat32, 16777216, 512, 0, 0);
    try std.testing.expectEqual(fmt.ok, fmt.priv_fmt_choose_geometry(&g, 0));
    try std.testing.expectEqual(@as(u32, 8), g.sectors_per_cluster);
    try std.testing.expectEqual(@as(u32, 16353), g.fat_size_sectors);
    try std.testing.expectEqual(@as(u32, 2093059), g.count_of_clusters);
}

test "too-small and undescribable devices fail cleanly" {
    var g = geomFor(fat16, 1000, 512, 512, 32);
    try std.testing.expectEqual(fmt.err_invalid_size, fmt.priv_fmt_choose_geometry(&g, 0));
    var h = geomFor(fat32, 4294967296, 512, 0, 0);
    try std.testing.expectEqual(fmt.err_invalid_size, fmt.priv_fmt_choose_geometry(&h, 0));
}

// A 100-sector fake disk, 512-byte sectors.
const Disk = struct {
    data: [100 * 512]u8 = undefined,
    writes: u32 = 0,
    fail_at: ?u32 = null,
};

fn writeBlock(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    const d: *Disk = @ptrCast(@alignCast(ctx.?));
    if (d.fail_at) |f| if (d.writes == f) return c.k_ra8_err_hw_error;
    const at: usize = @intCast(lba * 512);
    @memcpy(d.data[at..][0 .. count * 512], buf[0 .. count * 512]);
    d.writes += 1;
    return fmt.ok;
}

fn backendFor(d: *Disk) c.ra8_fs_backend_t {
    var b = std.mem.zeroes(c.ra8_fs_backend_t);
    b.ctx = d;
    b.write_block = writeBlock;
    return b;
}

test "emit_volume lays boot, seeds, FSInfo and backup on FAT32" {
    var d = Disk{};
    @memset(&d.data, 0xAA);
    const b = backendFor(&d);
    // Hand-built geometry on the cleared-span size: 32 reserved, two 4-sector
    // FAT copies, one root cluster.
    var g = geomFor(fat32, 100, 512, 0, 0);
    g.sectors_per_cluster = 1;
    g.fat_size_sectors = 4;
    g.reserved_sectors = 32;
    g.count_of_clusters = 60;
    try std.testing.expectEqual(fmt.ok, fmt.priv_fmt_emit_volume(&b, &g, null));

    const boot = d.data[0..512];
    try std.testing.expectEqual(@as(u8, 0xEB), boot[0]);
    try std.testing.expectEqual(@as(u8, 0x58), boot[1]);
    try std.testing.expectEqual(@as(u8, 0x90), boot[2]);
    try std.testing.expectEqualStrings("MSDOS5.0", boot[3..11]);
    try std.testing.expectEqual(@as(u16, 512), std.mem.readInt(u16, boot[11..13], .little));
    try std.testing.expectEqual(@as(u8, 1), boot[13]);
    try std.testing.expectEqual(@as(u16, 32), std.mem.readInt(u16, boot[14..16], .little));
    try std.testing.expectEqual(@as(u8, 2), boot[16]);
    try std.testing.expectEqual(@as(u32, 100), std.mem.readInt(u32, boot[32..36], .little));
    try std.testing.expectEqual(@as(u32, 4), std.mem.readInt(u32, boot[36..40], .little));
    try std.testing.expectEqual(@as(u32, 2), std.mem.readInt(u32, boot[44..48], .little));
    try std.testing.expectEqual(@as(u16, 1), std.mem.readInt(u16, boot[48..50], .little));
    try std.testing.expectEqual(@as(u16, 6), std.mem.readInt(u16, boot[50..52], .little));
    try std.testing.expectEqual(@as(u8, 0x29), boot[66]);
    try std.testing.expectEqual(@as(u32, 0x52A8D264), std.mem.readInt(u32, boot[67..71], .little));
    try std.testing.expectEqualStrings("NO NAME    ", boot[71..82]);
    try std.testing.expectEqualStrings("FAT32   ", boot[82..90]);
    try std.testing.expectEqual(@as(u8, 0x55), boot[510]);
    try std.testing.expectEqual(@as(u8, 0xAA), boot[511]);

    // FAT seeds in both copies: FAT[0]=0x0FFFFFF8, FAT[1]=FAT[2]=0x0FFFFFFF.
    for ([_]usize{ 32, 36 }) |fat_lba| {
        const fat = d.data[fat_lba * 512 ..][0..12];
        try std.testing.expectEqualSlices(u8, &.{ 0xF8, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF, 0xFF, 0x0F }, fat);
    }

    // FSInfo: lead + struct signatures, free count 59, next-free 3, trail.
    const fsi = d.data[512..][0..512];
    try std.testing.expectEqual(@as(u32, 0x41615252), std.mem.readInt(u32, fsi[0..4], .little));
    try std.testing.expectEqual(@as(u32, 0x61417272), std.mem.readInt(u32, fsi[484..488], .little));
    try std.testing.expectEqual(@as(u32, 59), std.mem.readInt(u32, fsi[488..492], .little));
    try std.testing.expectEqual(@as(u32, 3), std.mem.readInt(u32, fsi[492..496], .little));
    try std.testing.expectEqual(@as(u32, 0xAA550000), std.mem.readInt(u32, fsi[508..512], .little));

    // Backup boot matches the live boot sector.
    try std.testing.expectEqualSlices(u8, boot, d.data[6 * 512 ..][0..512]);

    // The clear span (reserved..reserved+2*4+1) is zero; past it is untouched.
    // Seed sectors carry the FAT[0]/[1]/[2] words; everything else is zero.
    for ([_]usize{ 32, 36 }) |fat_lba| {
        const fat = d.data[fat_lba * 512 ..][0..512];
        for (fat[12..]) |x| try std.testing.expectEqual(@as(u8, 0), x);
    }
    for ([_]usize{ 33, 34, 35, 37, 38, 39, 40 }) |lba| {
        for (d.data[lba * 512 ..][0..512]) |x| try std.testing.expectEqual(@as(u8, 0), x);
    }
    for (d.data[50 * 512 ..][0..512]) |x| try std.testing.expectEqual(@as(u8, 0xAA), x);
}

test "emit_volume stops at the first failed write" {
    var d = Disk{ .fail_at = 2 };
    const b = backendFor(&d);
    var g = geomFor(fat32, 100, 512, 0, 0);
    g.sectors_per_cluster = 1;
    g.fat_size_sectors = 4;
    g.reserved_sectors = 32;
    g.count_of_clusters = 60;
    const rc = fmt.priv_fmt_emit_volume(&b, &g, null);
    try std.testing.expect(rc != fmt.ok);
    try std.testing.expectEqual(@as(u32, 2), d.writes);
}
