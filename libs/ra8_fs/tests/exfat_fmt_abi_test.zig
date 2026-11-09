//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT format (mkfs) geometry + boot half (RA8FW), against a fake block
//! backend that records writes into a disk window.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const f = fs.exfat_fmt;
const st = fs.fat_state;

const ok: u16 = c.k_ra8_ok;
const invalid_size: u16 = c.k_ra8_err_invalid_size;

const bps: u32 = 512;
const part_lba: u64 = 100;

var disk: [256][512]u8 = @splat(@splat(0));
// A second window large enough for a real priv_exfat_format call: the
// partition sits at 1 MiB (LBA 2048) and the format touches through the
// root cluster around LBA 2170.
var disk2: [8192][512]u8 = @splat(@splat(0));

// The sector scratch pad the fmt ABI writes through (the C side's global).

fn fakeWrite(ctx: @TypeOf(@as(c.ra8_fs_backend_t, undefined).ctx), lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    _ = ctx;
    // The formatter's zero run streams multi-sector chunks (the C original
    // did the same), so accept any count and lay each sector in turn.
    if (count == 0 or lba + count > disk.len) {
        return 1;
    }
    var i: u64 = 0;
    while (i < count) : (i += 1) {
        var j: u32 = 0;
        while (j < bps) : (j += 1) {
            disk[lba + i][j] = buf[i * bps + j];
        }
    }
    return ok;
}

fn backend() c.ra8_fs_backend_t {
    var be = std.mem.zeroes(c.ra8_fs_backend_t);
    be.write_block = fakeWrite;
    return be;
}

fn fakeWrite2(ctx: @TypeOf(@as(c.ra8_fs_backend_t, undefined).ctx), lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    _ = ctx;
    if (count == 0 or lba + count > disk2.len) {
        return 1;
    }
    var i: u64 = 0;
    while (i < count) : (i += 1) {
        var j: u32 = 0;
        while (j < bps) : (j += 1) {
            disk2[lba + i][j] = buf[i * bps + j];
        }
    }
    return ok;
}

fn backend2() c.ra8_fs_backend_t {
    var be = std.mem.zeroes(c.ra8_fs_backend_t);
    be.write_block = fakeWrite2;
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
    // The deleted C folded conditionally: the wrap bit lands in the high
    // position only when the accumulator's low bit was set, so a zero seed
    // folds to the plain byte sum.
    const hibit: u32 = @intCast(c.k_exfat_fmt_csum_hibit);
    var one = [_]u8{0xFF};
    try std.testing.expectEqual(@as(u32, 0xFF), f.priv_exfat_csum32(0, &one, 1));
    var two = [_]u8{ 0xFF, 0x55 };
    try std.testing.expectEqual(hibit +% (@as(u32, 0xFF) >> 1) +% 0x55, f.priv_exfat_csum32(0, &two, 2));
    // A seed with the low bit set exercises the wrap bit.
    try std.testing.expectEqual(hibit +% 0xFF, f.priv_exfat_csum32(1, &one, 1));
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
    try std.testing.expectEqual(@as(u8, @intCast(c.k_exfat_fmt_jump0)), sec[@as(usize, @intCast(c.k_exfat_foff_jump))]);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_exfat_fmt_fs_rev)), @as(u16, sec[@as(usize, @intCast(c.k_exfat_foff_fs_rev))]) | (@as(u16, sec[@as(usize, @intCast(c.k_exfat_foff_fs_rev)) + 1]) << 8));
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_serial)) ^ @as(u32, 8192), rd32(&sec, @intCast(c.k_exfat_foff_serial)));
    try std.testing.expectEqual(@as(u32, 24), rd32(&sec, @intCast(c.k_exfat_off_fat_lba)));
    try std.testing.expectEqual(@as(u32, 5), rd32(&sec, @intCast(c.k_exfat_off_root_clus)));
    try std.testing.expectEqual(g.bps_shift, sec[@as(usize, @intCast(c.k_exfat_off_bps_shift))]);
    try std.testing.expectEqual(g.spc_shift, sec[@as(usize, @intCast(c.k_exfat_off_spc_shift))]);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_exfat_fmt_boot_sig)), @as(u16, sec[@as(usize, @intCast(c.k_exfat_foff_boot_sig))]) | (@as(u16, sec[@as(usize, @intCast(c.k_exfat_foff_boot_sig)) + 1]) << 8));
}

test "write boot lays the region and its backup" {
    disk = @splat(@splat(0));
    st.g_fs_scratch = @splat(0);
    var g = geom4m();
    try std.testing.expectEqual(ok, f.internal_exfat_geometry(8192, &g));
    const be = backend();
    try std.testing.expectEqual(ok, f.internal_exfat_write_boot(&be, &g));
    // VBR at the partition base and its backup 12 sectors later.
    try std.testing.expectEqualSlices(u8, &disk[100], &disk[112]);
    try std.testing.expectEqualStrings("EXFAT   ", disk[100][@intCast(c.k_exfat_off_fsname)..][0..8]);
    try std.testing.expectEqual(@as(u8, 0x55), disk[100][@as(usize, @intCast(c.k_exfat_foff_boot_sig))]);
    try std.testing.expectEqual(@as(u8, 0xAA), disk[100][@as(usize, @intCast(c.k_exfat_foff_boot_sig)) + 1]);
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

test "write fat seeds media, EOC and the pre-allocated chains" {
    disk = @splat(@splat(0));
    st.g_fs_scratch = @splat(0);
    var g = geom4m();
    try std.testing.expectEqual(ok, f.internal_exfat_geometry(8192, &g));
    const be = backend();
    try std.testing.expectEqual(ok, f.internal_exfat_write_fat(&be, &g));
    const fat = &disk[part_lba + g.fat_offset];
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_fat_media)), rd32(fat, 0));
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_fat_eoc)), rd32(fat, 4));
    // The bitmap chain is a single cluster (2) ending in EOC.
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_fat_eoc)), rd32(fat, 4 * g.upcase_cluster - 4));
    // The up-case chain links 3 -> 4 -> EOC.
    try std.testing.expectEqual(@as(u32, g.upcase_cluster + 1), rd32(fat, 4 * g.upcase_cluster));
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_fat_eoc)), rd32(fat, 4 * g.root_cluster - 4));
    // The root cluster ends in EOC and everything past it reads zero.
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_fat_eoc)), rd32(fat, 4 * g.root_cluster));
    try std.testing.expectEqual(@as(u32, 0), rd32(fat, 4 * (g.root_cluster + 1)));
    try std.testing.expectEqual(@as(u8, 0), fat[511]);
}

test "write bitmap marks the pre-allocated clusters" {
    disk = @splat(@splat(0));
    st.g_fs_scratch = @splat(0);
    var g = geom4m();
    try std.testing.expectEqual(ok, f.internal_exfat_geometry(8192, &g));
    const be = backend();
    try std.testing.expectEqual(ok, f.internal_exfat_write_bitmap(&be, &g));
    const bmp = &disk[part_lba + g.heap_offset];
    // used_clusters = 4: the low nibble of byte 0, nothing else.
    try std.testing.expectEqual(@as(u8, 0x0F), bmp[0]);
    try std.testing.expectEqual(@as(u8, 0), bmp[1]);
    try std.testing.expectEqual(@as(u8, 0), bmp[127]);
}

test "label utf16 packs ASCII and clamps at eleven" {
    var buf: [22]u8 = @splat(0xAA);
    try std.testing.expectEqual(@as(u32, 5), f.internal_exfat_label_utf16(&buf, "HELLO"));
    try std.testing.expectEqual(@as(u8, 'H'), buf[0]);
    try std.testing.expectEqual(@as(u8, 0), buf[1]);
    try std.testing.expectEqual(@as(u8, 'O'), buf[8]);
    try std.testing.expectEqual(@as(u8, 0), buf[9]);
    try std.testing.expectEqual(@as(u8, 0xAA), buf[10]); // untouched past the label
    try std.testing.expectEqual(@as(u32, 0), f.internal_exfat_label_utf16(&buf, null));
    try std.testing.expectEqual(@as(u32, 11), f.internal_exfat_label_utf16(&buf, "TWELVE CHARACTER"));
    try std.testing.expectEqual(@as(u8, 'R'), buf[20]); // the 11th unit's low byte
}

test "write root lays the three system entries and clears the cluster" {
    disk = @splat(@splat(0));
    st.g_fs_scratch = @splat(0);
    var g = geom4m();
    try std.testing.expectEqual(ok, f.internal_exfat_geometry(8192, &g));
    const be = backend();
    try std.testing.expectEqual(ok, f.internal_exfat_write_root(&be, &g, 0xDEADBEEF, "VOL"));
    const root = &disk[part_lba + g.heap_offset + (g.root_cluster - 2) * g.spc];
    const off2: usize = @intCast(c.k_exfat_de_second);
    const off3: usize = @intCast(c.k_exfat_de_third);
    try std.testing.expectEqual(@as(u8, 0x81), root[0]);
    try std.testing.expectEqual(@as(u32, 2), rd32(root, @intCast(c.k_exfat_de_first_clus)));
    try std.testing.expectEqual(g.bitmap_bytes, rd32(root, @intCast(c.k_exfat_de_data_len)));
    try std.testing.expectEqual(@as(u8, 0x82), root[off2]);
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), rd32(root, off2 + @as(usize, @intCast(c.k_exfat_de_upc_csum))));
    try std.testing.expectEqual(g.upcase_cluster, rd32(root, off2 + @as(usize, @intCast(c.k_exfat_de_first_clus))));
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_upc_std_bytes)), rd32(root, off2 + @as(usize, @intCast(c.k_exfat_de_data_len))));
    try std.testing.expectEqual(@as(u8, 0x83), root[off3]);
    try std.testing.expectEqual(@as(u8, 3), root[off3 + 1]);
    try std.testing.expectEqual(@as(u8, 'V'), root[off3 + 2]);
    try std.testing.expectEqual(@as(u8, 0), root[off3 + 3]);
    // The whole cluster reads as end-of-directory past the entries.
    try std.testing.expectEqual(@as(u8, 0), root[95]);
    try std.testing.expectEqual(@as(u8, 0), root[511]);
    try std.testing.expectEqual(@as(u8, 0), disk[part_lba + g.heap_offset + (g.root_cluster - 2) * g.spc + 1][0]);
    try std.testing.expectEqual(@as(u8, 0), disk[part_lba + g.heap_offset + (g.root_cluster - 2) * g.spc + 7][511]);
}

test "lba to chs packs the conventional geometry" {
    var chs: [3]u8 = @splat(0);
    f.internal_exfat_lba_to_chs(2048, &chs);
    // LBA 2048: cylinder 0, head 32, sector 33.
    try std.testing.expectEqual(@as(u8, 32), chs[0]);
    try std.testing.expectEqual(@as(u8, 33), chs[1]);
    try std.testing.expectEqual(@as(u8, 0), chs[2]);
    f.internal_exfat_lba_to_chs(63, &chs);
    // LBA 63: head 1, sector 1, cylinder 0.
    try std.testing.expectEqual(@as(u8, 1), chs[0]);
    try std.testing.expectEqual(@as(u8, 1), chs[1]);
    // Past the CHS space: the universal overflow sentinel.
    f.internal_exfat_lba_to_chs(@intCast(c.k_mbr_fmt_chs_max), &chs);
    try std.testing.expectEqual(@as(u8, @intCast(c.k_mbr_fmt_chs_ovf_h)), chs[0]);
    try std.testing.expectEqual(@as(u8, @intCast(c.k_mbr_fmt_chs_ovf_m)), chs[1]);
    try std.testing.expectEqual(@as(u8, @intCast(c.k_mbr_fmt_chs_ovf_l)), chs[2]);
}

test "write mbr stamps the signature and one exFAT entry" {
    disk = @splat(@splat(0));
    st.g_fs_scratch = @splat(0);
    const be = backend();
    try std.testing.expectEqual(ok, f.internal_exfat_write_mbr(&be, bps, 2048, 65536));
    const m = &disk[0];
    const pe: usize = @intCast(c.k_mbr_fmt_part0_off);
    try std.testing.expectEqual(@as(u32, @as(u32, @intCast(c.k_mbr_fmt_disk_sig_base)) ^ 65536), rd32(m, @intCast(c.k_mbr_fmt_disk_sig_off)));
    try std.testing.expectEqual(@as(u8, @intCast(c.k_mbr_fmt_boot_none)), m[pe]);
    try std.testing.expectEqual(@as(u8, @intCast(c.k_mbr_fmt_type_exfat)), m[pe + 4]);
    try std.testing.expectEqual(@as(u32, 2048), rd32(m, pe + 8));
    try std.testing.expectEqual(@as(u32, 65536), rd32(m, pe + 12));
    try std.testing.expectEqual(@as(u8, 0x55), m[@as(usize, @intCast(c.k_exfat_foff_boot_sig))]);
    try std.testing.expectEqual(@as(u8, 0xAA), m[@as(usize, @intCast(c.k_exfat_foff_boot_sig)) + 1]);
}

test "fmt place sizes the partition and refuses the edges" {
    const p = f.internal_exfat_fmt_place(67584, bps).?;
    try std.testing.expectEqual(@as(u8, 9), p.bps_shift);
    try std.testing.expectEqual(@as(u32, 2048), p.part_lba);
    try std.testing.expectEqual(@as(u32, 65536), p.part_sectors);
    try std.testing.expect(f.internal_exfat_fmt_place(67583, bps) == null); // below the minimum volume
    try std.testing.expect(f.internal_exfat_fmt_place(4294969344, bps) == null); // past the MBR's 32-bit fields
}

test "priv exfat format lays the whole volume end to end" {
    disk2 = @splat(@splat(0));
    st.g_fs_scratch = @splat(0);
    const be = backend2();
    try std.testing.expectEqual(ok, f.priv_exfat_format(&be, 67584, bps, "RA8VOL"));
    // MBR at LBA 0 with the exFAT entry.
    try std.testing.expectEqual(@as(u8, 0x07), disk2[0][@as(usize, @intCast(c.k_mbr_fmt_part0_off)) + 4]);
    // Boot region at the partition base, VBR == backup 12 sectors later.
    try std.testing.expectEqualStrings("EXFAT   ", disk2[2048][@intCast(c.k_exfat_off_fsname)..][0..8]);
    try std.testing.expectEqualSlices(u8, &disk2[2048], &disk2[2060]);
    var g = geom4m();
    try std.testing.expectEqual(ok, f.internal_exfat_geometry(65536, &g));
    // FAT seeded at fat_offset.
    const fat = &disk2[2048 + g.fat_offset];
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_fat_media)), rd32(fat, 0));
    try std.testing.expectEqual(@as(u32, @intCast(c.k_exfat_fmt_fat_eoc)), rd32(fat, 4));
    // Bitmap marked with the four pre-allocated clusters.
    try std.testing.expectEqual(@as(u8, 0x0F), disk2[2048 + g.heap_offset][0]);
    // Up-case table written at its cluster, first word 0x0000.
    const upc_lba = 2048 + g.heap_offset + (g.upcase_cluster - 2) * g.spc;
    try std.testing.expectEqual(@as(u8, 0x00), disk2[upc_lba][0]);
    try std.testing.expectEqual(@as(u8, 0x00), disk2[upc_lba][1]);
    // Root directory: three entries, and the up-case checksum matches the
    // canonical table's checksum.
    const root = &disk2[2048 + g.heap_offset + (g.root_cluster - 2) * g.spc];
    try std.testing.expectEqual(@as(u8, 0x81), root[0]);
    try std.testing.expectEqual(@as(u8, 0x82), root[32]);
    try std.testing.expectEqual(@as(u8, 0x83), root[64]);
    try std.testing.expectEqual(fs.upcase.priv_exfat_upcase_checksum(), rd32(root, 36));
    try std.testing.expectEqual(@as(u8, 6), root[65]); // label character count
    try std.testing.expectEqual(@as(u8, 'R'), root[66]);
    try std.testing.expectEqual(@as(u8, 0), root[95]);
    try std.testing.expectEqual(@as(u8, 0), disk2[2048 + g.heap_offset + (g.root_cluster - 2) * g.spc + 1][0]);
}
