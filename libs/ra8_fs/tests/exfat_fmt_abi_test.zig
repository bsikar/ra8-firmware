//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT format (mkfs) geometry + boot half (RA8FW), against a fake block
//! backend that records writes into a disk window.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const f = fs.exfat_fmt;

const ok: u16 = c.k_ra8_ok;
const invalid_size: u16 = c.k_ra8_err_invalid_size;

const bps: u32 = 512;
const part_lba: u64 = 100;

var disk: [128][512]u8 = @splat(@splat(0));

// The sector scratch pad the fmt ABI writes through (the C side's global).
export var g_fs_scratch: [c.k_ra8_fs_sector_max]u8 = @splat(0);

fn fakeWrite(ctx: @TypeOf(@as(c.ra8_fs_backend_t, undefined).ctx), lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    _ = ctx;
    if (lba >= disk.len or count != 1) {
        return 1;
    }
    var i: u32 = 0;
    while (i < bps) : (i += 1) {
        disk[lba][i] = buf[i];
    }
    return ok;
}

fn backend() c.ra8_fs_backend_t {
    var be = std.mem.zeroes(c.ra8_fs_backend_t);
    be.write_block = fakeWrite;
    return be;
}

fn geom4m() f.Geom {
    var g = std.mem.zeroes(f.Geom);
    g.bps = bps;
    g.bps_shift = 9;
    g.part_lba = part_lba;
    return g;
}

fn rd32(buf: []const u8, off: usize) u32 {
    return @as(u32, buf[off]) | (@as(u32, buf[off + 1]) << 8) | (@as(u32, buf[off + 2]) << 16) | (@as(u32, buf[off + 3]) << 24);
}

test "csum32 folds the exFAT checksum" {
    const hibit: u32 = @intCast(c.k_exfat_fmt_csum_hibit);
    var one = [_]u8{0xFF};
    try std.testing.expectEqual(hibit +% 0xFF, f.priv_exfat_csum32(0, &one, 1));
    var two = [_]u8{ 0xFF, 0x55 };
    const step1 = hibit +% 0xFF;
    try std.testing.expectEqual(((hibit +% (step1 >> 1)) +% 0x55), f.priv_exfat_csum32(0, &two, 2));
    try std.testing.expectEqual(@as(u32, 0), f.priv_exfat_csum32(0, &one, 0));
}

test "spc shift follows the size tiers" {
    // 512-byte sectors: 256 MB -> 4 kB clusters, 32 GB -> 32 kB, 256 GB ->
    // 128 kB, larger -> 256 kB.
    try std.testing.expectEqual(@as(u8, 3), f.internal_exfat_spc_shift(524288, 9));
    try std.testing.expectEqual(@as(u8, 6), f.internal_exfat_spc_shift(67108864, 9));
    try std.testing.expectEqual(@as(u8, 8), f.internal_exfat_spc_shift(536870912, 9));
    try std.testing.expectEqual(@as(u8, 9), f.internal_exfat_spc_shift(536870913, 9));
    // A 4Kn device: 4 kB clusters equal one sector, so the shift is 0.
    try std.testing.expectEqual(@as(u8, 0), f.internal_exfat_spc_shift(65536, 12));
}

test "geometry lays out a 4 MB volume" {
    var g = geom4m();
    try std.testing.expectEqual(ok, f.internal_exfat_geometry(8192, &g));
    try std.testing.expectEqual(@as(u8, 3), g.spc_shift);
    try std.testing.expectEqual(@as(u32, 8), g.spc);
    try std.testing.expectEqual(@as(u32, 24), g.fat_offset);
    try std.testing.expectEqual(@as(u32, 8), g.fat_length);
    try std.testing.expectEqual(@as(u32, 32), g.heap_offset);
    try std.testing.expectEqual(@as(u32, 1020), g.cluster_count);
    try std.testing.expectEqual(@as(u32, 128), g.bitmap_bytes);
    try std.testing.expectEqual(@as(u32, 1), g.bitmap_clusters);
    try std.testing.expectEqual(@as(u32, 2), g.upcase_clusters);
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_first_clus)) + 1, g.upcase_cluster);
    try std.testing.expectEqual(g.upcase_cluster + g.upcase_clusters, g.root_cluster);
    try std.testing.expectEqual(@as(u32, 4), g.used_clusters);
}

test "geometry rejects devices too small for a volume" {
    var g = geom4m();
    try std.testing.expectEqual(invalid_size, f.internal_exfat_geometry(24, &g));
    try std.testing.expectEqual(invalid_size, f.internal_exfat_geometry(25, &g));
}

test "build vbr writes the spec fields" {
    var g = geom4m();
    try std.testing.expectEqual(ok, f.internal_exfat_geometry(8192, &g));
    var sec: [512]u8 = @splat(0xFF);
    f.internal_exfat_build_vbr(&sec, &g);
    try std.testing.expectEqualStrings("EXFAT   ", sec[@intCast(c.k_exfat_off_fsname)..][0..8]);
    try std.testing.expectEqual(@as(u8, @intCast(c.k_exfat_fmt_jump0)), sec[@intCast(c.k_exfat_foff_jump)]);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_exfat_fmt_fs_rev)), @as(u16, sec[@intCast(c.k_exfat_foff_fs_rev)]) | (@as(u16, sec[@intCast(c.k_exfat_foff_fs_rev) + 1]) << 8));
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_serial)) ^ @as(u32, 8192), rd32(&sec, @intCast(c.k_exfat_foff_serial)));
    try std.testing.expectEqual(@as(u32, 24), rd32(&sec, @intCast(c.k_exfat_off_fat_lba)));
    try std.testing.expectEqual(@as(u32, 5), rd32(&sec, @intCast(c.k_exfat_off_root_clus)));
    try std.testing.expectEqual(g.bps_shift, sec[@intCast(c.k_exfat_off_bps_shift)]);
    try std.testing.expectEqual(g.spc_shift, sec[@intCast(c.k_exfat_off_spc_shift)]);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_exfat_fmt_boot_sig)), @as(u16, sec[@intCast(c.k_exfat_foff_boot_sig)]) | (@as(u16, sec[@intCast(c.k_exfat_foff_boot_sig) + 1]) << 8));
}

test "write boot lays the region and its backup" {
    disk = @splat(@splat(0));
    g_fs_scratch = @splat(0);
    var g = geom4m();
    try std.testing.expectEqual(ok, f.internal_exfat_geometry(8192, &g));
    const be = backend();
    try std.testing.expectEqual(ok, f.internal_exfat_write_boot(&be, &g));
    // VBR at the partition base and its backup 12 sectors later.
    try std.testing.expectEqualSlices(u8, &disk[100], &disk[112]);
    try std.testing.expectEqualStrings("EXFAT   ", disk[100][@intCast(c.k_exfat_off_fsname)..][0..8]);
    try std.testing.expectEqual(@as(u8, 0x55), disk[100][@intCast(c.k_exfat_foff_boot_sig)]);
    try std.testing.expectEqual(@as(u8, 0xAA), disk[100][@intCast(c.k_exfat_foff_boot_sig) + 1]);
    // Ext-boot sectors 1-8 carry the ext signature in the last four bytes.
    var s: u32 = @intCast(c.k_exfat_fmt_ext_first);
    while (s < @as(u32, @intCast(c.k_exfat_fmt_ext_first)) + @as(u32, @intCast(c.k_exfat_fmt_ext_count))) : (s += 1) {
        try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_ext_sig)), rd32(&disk[100 + s], 508));
    }
    // OEM + reserved are all zero.
    try std.testing.expectEqual(@as(u8, 0), disk[109][0]);
    try std.testing.expectEqual(@as(u8, 0), disk[110][511]);
    // Checksum sector: every word equals the final boot checksum, and the
    // backup copy matches.
    const cs_word = rd32(&disk[111], 0);
    try std.testing.expect(cs_word != 0);
    var i: usize = 0;
    while (i < 128) : (i += 1) {
        try std.testing.expectEqual(cs_word, rd32(&disk[111], i * 4));
    }
    try std.testing.expectEqualSlices(u8, &disk[111], &disk[123]);
    // Every boot sector landed twice: 24 writes for sectors 0-11.
}
