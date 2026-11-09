//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT format (mkfs), complete: the running boot checksum, the
//! cluster-size tier lookup, the volume geometry resolver, the Main Boot
//! Sector builder, the writer that lands boot sectors 0-11 and their backups,
//! and the FAT/bitmap/root/MBR half with the `priv_exfat_format`
//! orchestrator. Ported from `ra8_fs_fat_exfat_fmt.c` across two slices
//! (RA8FW-1121 boot half, RA8FW-1122 the rest); the C file is a stub.

const std = @import("std");
const c = @import("fs_c.zig").c;

const ok: u16 = c.k_ra8_ok;
const invalid_size: u16 = c.k_ra8_err_invalid_size;

const fmt_boot_secs: u32 = @intCast(c.k_exfat_fmt_boot_secs);
const fmt_byte_bits: u32 = @intCast(c.k_exfat_fmt_byte_bits);
const fmt_upc_std_bytes: u32 = @intCast(c.k_exfat_fmt_upc_std_bytes);
const fmt_first_clus: u32 = @intCast(c.k_exfat_fmt_first_clus);
const fmt_geom_iters: u32 = @intCast(c.k_exfat_fmt_geom_iters);
const fmt_backup_lba: u32 = @intCast(c.k_exfat_fmt_backup_lba);
const fmt_ext_first: u32 = @intCast(c.k_exfat_fmt_ext_first);
const fmt_ext_count: u32 = @intCast(c.k_exfat_fmt_ext_count);
const fmt_oem_lba: u32 = @intCast(c.k_exfat_fmt_oem_lba);
const fmt_resv_lba: u32 = @intCast(c.k_exfat_fmt_resv_lba);
const fmt_csum_lba: u32 = @intCast(c.k_exfat_fmt_csum_lba);
const fmt_csum_skip0: u32 = @intCast(c.k_exfat_fmt_csum_skip0);
const fmt_csum_skip2: u32 = @intCast(c.k_exfat_fmt_csum_skip2);
const sector_min: u32 = @intCast(c.k_ra8_fs_sector_min);
const bps_shift_min: u8 = @intCast(c.k_exfat_bps_shift_min);

extern var g_fs_scratch: [c.k_ra8_fs_sector_max]u8;
extern fn priv_wr16(p: [*c]u8, v: u16) callconv(.c) void;
extern fn priv_wr32(p: [*c]u8, v: u32) callconv(.c) void;
extern fn priv_wr64(p: [*c]u8, v: u64) callconv(.c) void;

/// The C half's geometry record; an extern struct so a pointer handed from
/// either side reads the same bytes.
pub const Geom = extern struct {
    part_lba: u64,
    total_sectors: u64,
    bps: u32,
    bps_shift: u8,
    spc_shift: u8,
    spc: u32,
    fat_offset: u32,
    fat_length: u32,
    heap_offset: u32,
    cluster_count: u32,
    bitmap_bytes: u32,
    bitmap_clusters: u32,
    upcase_cluster: u32,
    upcase_clusters: u32,
    root_cluster: u32,
    used_clusters: u32,
};

/// exFAT running checksum: for every byte, the odd bit becomes the table
/// high bit, the accumulator halves, and the byte folds in.
pub export fn priv_exfat_csum32(cs: u32, buf: [*c]const u8, len: u32) callconv(.c) u32 {
    const hibit: u32 = @intCast(c.k_exfat_fmt_csum_hibit);
    var acc = cs;
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        acc = ((if ((acc & 1) != 0) hibit else 0) +% (acc >> 1)) +% @as(u32, buf[i]);
    }
    return acc;
}

/// Default SectorsPerClusterShift per the spec's size tiers, expressed in
/// cluster bytes so every sector size lands on the same cluster size a
/// 512-byte device of equal capacity gets.
pub export fn internal_exfat_spc_shift(total_sectors: u64, bps_shift: u8) callconv(.c) u8 {
    // The subtraction is guarded even though every caller passes 9..12: the
    // analyzer cannot see the cross-function bound, and an under-9 shift here
    // would be a 2^32-sized left shift rather than a wrong answer.
    const scale: u6 = if (bps_shift > bps_shift_min) @intCast(bps_shift - bps_shift_min) else 0;
    const total_512: u64 = total_sectors << scale;
    var clus_shift: u32 = @intCast(c.k_exfat_fmt_clus_256k);
    if (total_512 <= c.k_exfat_fmt_thr_256m) {
        clus_shift = @intCast(c.k_exfat_fmt_clus_4k);
    } else if (total_512 <= c.k_exfat_fmt_thr_32g) {
        clus_shift = @intCast(c.k_exfat_fmt_clus_32k);
    } else if (total_512 <= c.k_exfat_fmt_thr_256g) {
        clus_shift = @intCast(c.k_exfat_fmt_clus_128k);
    }
    if (clus_shift <= bps_shift) {
        return 0;
    }
    return @intCast(clus_shift - bps_shift);
}

/// Resolve the exFAT region geometry: pick the cluster size, fixed-point the
/// FAT length against the cluster count, then lay the bitmap, up-case table,
/// and root directory as the first contiguous clusters of the heap.
pub export fn internal_exfat_geometry(total_sectors: u64, g: [*c]Geom) callconv(.c) u16 {
    g[0].total_sectors = total_sectors;
    g[0].spc_shift = internal_exfat_spc_shift(total_sectors, g[0].bps_shift);
    g[0].spc = @as(u32, 1) << @intCast(g[0].spc_shift);
    g[0].fat_offset = fmt_boot_secs;
    if (total_sectors <= g[0].fat_offset) {
        return invalid_size;
    }
    var clusters: u32 = @intCast((total_sectors - g[0].fat_offset) / g[0].spc);
    g[0].fat_length = 0;
    g[0].heap_offset = 0;
    var i: u32 = 0;
    while (i < fmt_geom_iters) : (i += 1) {
        const fat_bytes: u64 = (@as(u64, clusters) + 2) * 4;
        const fat_len: u32 = @intCast((fat_bytes + g[0].bps - 1) / g[0].bps);
        const heap: u32 = g[0].fat_offset + fat_len;
        if (total_sectors <= heap) {
            return invalid_size;
        }
        const next: u32 = @intCast((total_sectors - heap) / g[0].spc);
        g[0].fat_length = fat_len;
        g[0].heap_offset = heap;
        if (next == clusters) {
            break;
        }
        clusters = next;
    }
    g[0].cluster_count = @intCast((total_sectors - g[0].heap_offset) / g[0].spc);
    const cluster_bytes: u32 = g[0].spc * g[0].bps;
    g[0].bitmap_bytes = (g[0].cluster_count + fmt_byte_bits - 1) / fmt_byte_bits;
    g[0].bitmap_clusters = (g[0].bitmap_bytes + cluster_bytes - 1) / cluster_bytes;
    g[0].upcase_clusters = (fmt_upc_std_bytes + cluster_bytes - 1) / cluster_bytes;
    g[0].upcase_cluster = fmt_first_clus + g[0].bitmap_clusters;
    g[0].root_cluster = g[0].upcase_cluster + g[0].upcase_clusters;
    g[0].used_clusters = g[0].bitmap_clusters + g[0].upcase_clusters + 1; // bitmap + up-case + root
    if (g[0].cluster_count < g[0].used_clusters) {
        return invalid_size;
    }
    return ok;
}

/// Build the 512-byte exFAT Main Boot Sector: zero the sector, then write
/// every mandatory VBR field as little-endian per the Microsoft spec.
pub export fn internal_exfat_build_vbr(sec: [*c]u8, g: [*c]const Geom) callconv(.c) void {
    var i: u32 = 0;
    while (i < g[0].bps) : (i += 1) {
        sec[i] = 0;
    }
    sec[@as(u32, @intCast(c.k_exfat_foff_jump))] = @intCast(c.k_exfat_fmt_jump0);
    sec[@as(u32, @intCast(c.k_exfat_foff_jump)) + 1] = @intCast(c.k_exfat_fmt_jump1);
    sec[@as(u32, @intCast(c.k_exfat_foff_jump)) + 2] = @intCast(c.k_exfat_fmt_jump2);
    const nm = "EXFAT   ";
    i = 0;
    while (i < @as(u32, @intCast(c.k_exfat_fsname_len))) : (i += 1) {
        sec[@as(u32, @intCast(c.k_exfat_off_fsname)) + i] = nm[i];
    }
    priv_wr64(sec + @as(u32, @intCast(c.k_exfat_foff_part_off)), g[0].part_lba);
    priv_wr64(sec + @as(u32, @intCast(c.k_exfat_foff_vol_len)), g[0].total_sectors);
    priv_wr32(sec + @as(u32, @intCast(c.k_exfat_off_fat_lba)), g[0].fat_offset);
    priv_wr32(sec + @as(u32, @intCast(c.k_exfat_off_fat_len)), g[0].fat_length);
    priv_wr32(sec + @as(u32, @intCast(c.k_exfat_off_heap_lba)), g[0].heap_offset);
    priv_wr32(sec + @as(u32, @intCast(c.k_exfat_off_clus_count)), g[0].cluster_count);
    priv_wr32(sec + @as(u32, @intCast(c.k_exfat_off_root_clus)), g[0].root_cluster);
    priv_wr32(sec + @as(u32, @intCast(c.k_exfat_foff_serial)), @as(u32, @intCast(c.k_exfat_fmt_serial)) ^ @as(u32, @truncate(g[0].total_sectors)));
    priv_wr16(sec + @as(u32, @intCast(c.k_exfat_foff_fs_rev)), @intCast(c.k_exfat_fmt_fs_rev));
    sec[@intCast(c.k_exfat_off_bps_shift)] = g[0].bps_shift;
    sec[@intCast(c.k_exfat_off_spc_shift)] = g[0].spc_shift;
    sec[@intCast(c.k_exfat_off_num_fats)] = @intCast(c.k_exfat_fmt_num_fats);
    sec[@intCast(c.k_exfat_foff_drive)] = @intCast(c.k_exfat_fmt_drive);
    sec[@intCast(c.k_exfat_foff_percent)] = @intCast(c.k_exfat_fmt_percent);
    priv_wr16(sec + @as(u32, @intCast(c.k_exfat_foff_boot_sig)), @intCast(c.k_exfat_fmt_boot_sig));
}

/// Write one sector to both its primary location and its backup one
/// `k_exfat_fmt_backup_lba` sectors later; the first failure aborts so the
/// backup write is skipped rather than half-applied.
pub export fn internal_exfat_wr_dual(b: [*c]const c.ra8_fs_backend_t, part_lba: u64, lba: u32, buf: [*c]const u8) callconv(.c) u16 {
    const err = b[0].write_block.?(b[0].ctx, part_lba +% lba, 1, buf);
    if (err != ok) {
        return err;
    }
    return b[0].write_block.?(b[0].ctx, part_lba +% lba +% fmt_backup_lba, 1, buf);
}

/// Store a 32-bit little-endian word at 32-bit-word slot `idx` of a sector.
pub export fn internal_exfat_put32(buf: [*c]u8, idx: u32, val: u32) callconv(.c) void {
    priv_wr32(buf + (idx * 4), val);
}

/// Write boot sectors 1-11 (ext-boot, OEM, reserved, checksum) to main and
/// backup, folding each into the running boot checksum as it goes.
pub export fn internal_exfat_write_boot_tail(backend: [*c]const c.ra8_fs_backend_t, g: [*c]const Geom, cs_in: u32) callconv(.c) u16 {
    var cs = cs_in;
    // Sectors 1-8: extended boot (ext-boot signature only). The extended
    // boot signature sits in the sector's LAST four bytes (exFAT spec 3.2).
    var i: u32 = 0;
    while (i < g[0].bps) : (i += 1) {
        g_fs_scratch[i] = 0;
    }
    priv_wr32(&g_fs_scratch[g[0].bps - (sector_min - @as(u32, @intCast(c.k_exfat_foff_ext_sig)))], @intCast(c.k_exfat_fmt_ext_sig));
    var s: u32 = fmt_ext_first;
    while (s < fmt_ext_first + fmt_ext_count) : (s += 1) {
        cs = priv_exfat_csum32(cs, &g_fs_scratch, g[0].bps);
        const e = internal_exfat_wr_dual(backend, g[0].part_lba, s, &g_fs_scratch);
        if (e != ok) {
            return e;
        }
    }
    // Sectors 9 (OEM) + 10 (reserved): all zero.
    i = 0;
    while (i < g[0].bps) : (i += 1) {
        g_fs_scratch[i] = 0;
    }
    cs = priv_exfat_csum32(cs, &g_fs_scratch, g[0].bps);
    var err = internal_exfat_wr_dual(backend, g[0].part_lba, fmt_oem_lba, &g_fs_scratch);
    if (err != ok) {
        return err;
    }
    cs = priv_exfat_csum32(cs, &g_fs_scratch, g[0].bps);
    err = internal_exfat_wr_dual(backend, g[0].part_lba, fmt_resv_lba, &g_fs_scratch);
    if (err != ok) {
        return err;
    }
    // Checksum sector: every 32-bit word = the boot checksum.
    i = 0;
    while (i < g[0].bps / 4) : (i += 1) {
        internal_exfat_put32(&g_fs_scratch, i, cs);
    }
    return internal_exfat_wr_dual(backend, g[0].part_lba, fmt_csum_lba, &g_fs_scratch);
}

/// Write the complete exFAT main and backup boot regions (sectors 0-23):
/// build the VBR, fold the boot checksum over it skipping the VolumeFlags
/// and PercentInUse bytes, land sector 0, then the rest of the region.
pub export fn internal_exfat_write_boot(backend: [*c]const c.ra8_fs_backend_t, g: [*c]const Geom) callconv(.c) u16 {
    internal_exfat_build_vbr(&g_fs_scratch, g);
    // Boot checksum over sector 0 skips bytes 106,107 (VolumeFlags) and 112
    // (PercentInUse): fold [0,106), [106,112), [113,bps).
    var cs = priv_exfat_csum32(0, &g_fs_scratch, fmt_csum_skip0);
    cs = priv_exfat_csum32(cs, &g_fs_scratch[@intCast(c.k_exfat_off_bps_shift)], fmt_csum_skip2 - @as(u32, @intCast(c.k_exfat_off_bps_shift)));
    cs = priv_exfat_csum32(cs, &g_fs_scratch[fmt_csum_skip2 + 1], g[0].bps - (fmt_csum_skip2 + 1));
    const err = internal_exfat_wr_dual(backend, g[0].part_lba, 0, &g_fs_scratch);
    if (err != ok) {
        return err;
    }
    return internal_exfat_write_boot_tail(backend, g, cs);
}

// ---- FAT, bitmap, root and MBR half (RA8FW-1122) ----

const fmt_fat_media: u32 = @intCast(c.k_exfat_fmt_fat_media);
const fmt_fat_eoc: u32 = @intCast(c.k_exfat_fmt_fat_eoc);
const fmt_byte_full: u8 = @intCast(c.k_exfat_fmt_byte_full);
const fmt_label_max: u32 = @intCast(c.k_exfat_fmt_label_max);
const fmt_part_align: u32 = @intCast(c.k_exfat_fmt_part_align);
const fmt_min_sectors: u32 = @intCast(c.k_exfat_fmt_min_sectors);
const entry_bitmap: u8 = @intCast(c.k_exfat_entry_bitmap);
const entry_upcase: u8 = @intCast(c.k_exfat_entry_upcase);
const entry_label: u8 = @intCast(c.k_exfat_entry_label);
const de_second: u32 = @intCast(c.k_exfat_de_second);
const de_third: u32 = @intCast(c.k_exfat_de_third);
const de_first_clus: u32 = @intCast(c.k_exfat_de_first_clus);
const de_data_len: u32 = @intCast(c.k_exfat_de_data_len);
const de_upc_csum: u32 = @intCast(c.k_exfat_de_upc_csum);
const de_lbl_cnt: u32 = @intCast(c.k_exfat_de_lbl_cnt);
const de_lbl_name: u32 = @intCast(c.k_exfat_de_lbl_name);
const boot_sig_off: u32 = @intCast(c.k_exfat_foff_boot_sig);
const mbr_disk_sig_off: u32 = @intCast(c.k_mbr_fmt_disk_sig_off);
const mbr_disk_sig_base: u32 = @intCast(c.k_mbr_fmt_disk_sig_base);
const mbr_part0_off: u32 = @intCast(c.k_mbr_fmt_part0_off);
const mbr_pe_boot: u32 = @intCast(c.k_mbr_fmt_pe_boot);
const mbr_pe_chs_start: u32 = @intCast(c.k_mbr_fmt_pe_chs_start);
const mbr_pe_type: u32 = @intCast(c.k_mbr_fmt_pe_type);
const mbr_pe_chs_end: u32 = @intCast(c.k_mbr_fmt_pe_chs_end);
const mbr_pe_lba: u32 = @intCast(c.k_mbr_fmt_pe_lba);
const mbr_pe_nsect: u32 = @intCast(c.k_mbr_fmt_pe_nsect);
const mbr_boot_none: u8 = @intCast(c.k_mbr_fmt_boot_none);
const mbr_type_exfat: u8 = @intCast(c.k_mbr_fmt_type_exfat);
const mbr_chs_heads: u32 = @intCast(c.k_mbr_fmt_chs_heads);
const mbr_chs_spt: u32 = @intCast(c.k_mbr_fmt_chs_spt);
const mbr_chs_max: u32 = @intCast(c.k_mbr_fmt_chs_max);
const mbr_chs_ovf_h: u8 = @intCast(c.k_mbr_fmt_chs_ovf_h);
const mbr_chs_ovf_m: u8 = @intCast(c.k_mbr_fmt_chs_ovf_m);
const mbr_chs_ovf_l: u8 = @intCast(c.k_mbr_fmt_chs_ovf_l);
const mbr_chs_sec_mask: u32 = @intCast(c.k_mbr_fmt_chs_sec_mask);
const mbr_chs_cyl_hi_mask: u32 = @intCast(c.k_mbr_fmt_chs_cyl_hi_mask);
const mbr_chs_cyl_hi_shift: u32 = @intCast(c.k_mbr_fmt_chs_cyl_hi_shift);
const byte_mask: u32 = @intCast(c.k_byte_mask);
const not_supported: u16 = @intCast(c.k_ra8_err_not_supported);

extern fn priv_fmt_clear_region(backend: [*c]const c.ra8_fs_backend_t, lba: u64, count: u64, bps: u32) callconv(.c) u16;
extern fn priv_exfat_write_upcase(backend: [*c]const c.ra8_fs_backend_t, abs_lba: u64, bps: u32, out_csum: *u32) callconv(.c) u16;

/// Zero the exFAT FAT region, then seed FAT sector 0: FAT[0] media, FAT[1]
/// EOC, the bitmap and up-case chains as linked runs terminating in EOC, and
/// the root cluster as a single EOC entry. The orchestrator's root-cluster
/// guard keeps every word inside sector 0.
pub fn internal_exfat_write_fat(backend: [*c]const c.ra8_fs_backend_t, g: *const Geom) u16 {
    const err = priv_fmt_clear_region(backend, g.part_lba +% g.fat_offset, g.fat_length, g.bps);
    if (err != ok) {
        return err;
    }
    @memset(g_fs_scratch[0..g.bps], 0);
    internal_exfat_put32(&g_fs_scratch, 0, fmt_fat_media);
    internal_exfat_put32(&g_fs_scratch, 1, fmt_fat_eoc);
    const bmp_last = fmt_first_clus + g.bitmap_clusters - 1;
    var clus = fmt_first_clus;
    while (clus <= bmp_last) : (clus += 1) {
        internal_exfat_put32(&g_fs_scratch, clus, if (clus < bmp_last) clus + 1 else fmt_fat_eoc);
    }
    const upc_last = g.upcase_cluster + g.upcase_clusters - 1;
    clus = g.upcase_cluster;
    while (clus <= upc_last) : (clus += 1) {
        internal_exfat_put32(&g_fs_scratch, clus, if (clus < upc_last) clus + 1 else fmt_fat_eoc);
    }
    internal_exfat_put32(&g_fs_scratch, g.root_cluster, fmt_fat_eoc);
    return backend[0].write_block.?(backend[0].ctx, g.part_lba +% g.fat_offset, 1, &g_fs_scratch);
}

/// Zero the allocation-bitmap region, then mark the pre-allocated clusters
/// in its first sector: full bytes 0xFF, a trailing partial byte gets a mask
/// of the remaining bits.
pub fn internal_exfat_write_bitmap(backend: [*c]const c.ra8_fs_backend_t, g: *const Geom) u16 {
    const bmp_lba = g.part_lba +% g.heap_offset;
    const bmp_sectors = g.bitmap_clusters * g.spc;
    const err = priv_fmt_clear_region(backend, bmp_lba, bmp_sectors, g.bps);
    if (err != ok) {
        return err;
    }
    @memset(g_fs_scratch[0..g.bps], 0);
    // A valid geometry keeps the pre-allocated marks inside the first
    // bitmap sector; the bound keeps a broken one from walking past it.
    const full = @min(g.used_clusters / fmt_byte_bits, g.bps);
    var i: u32 = 0;
    while (i < full) : (i += 1) {
        g_fs_scratch[i] = fmt_byte_full;
    }
    const rem = g.used_clusters % fmt_byte_bits;
    if (rem != 0 and full < g.bps) {
        g_fs_scratch[full] = @intCast((@as(u32, 1) << @intCast(rem)) - 1);
    }
    return backend[0].write_block.?(backend[0].ctx, bmp_lba, 1, &g_fs_scratch);
}

/// Pack an ASCII volume label into UTF-16LE for the Volume Label entry:
/// zero-extend each byte, stop at NUL or the 11-unit cap; a null label
/// writes nothing and returns 0.
pub fn internal_exfat_label_utf16(dst: [*c]u8, label: [*c]const u8) u32 {
    var n: u32 = 0;
    if (label != null) {
        while (n < fmt_label_max and label[n] != 0) : (n += 1) {
            dst[n * 2] = label[n];
            dst[n * 2 + 1] = 0;
        }
    }
    return n;
}

/// Write the root directory sector: an Allocation Bitmap entry, an Up-case
/// Table entry, and a Volume Label entry. The whole root cluster is zeroed
/// first, not just this sector, so anything scanning past the three entries
/// reads end-of-directory rather than pre-format device data.
pub fn internal_exfat_write_root(backend: [*c]const c.ra8_fs_backend_t, g: *const Geom, upcase_csum: u32, label: [*c]const u8) u16 {
    const lba = @as(u64, g.heap_offset) + @as(u64, g.root_cluster - fmt_first_clus) * g.spc;
    const ze = priv_fmt_clear_region(backend, g.part_lba +% lba, g.spc, g.bps);
    if (ze != ok) {
        return ze;
    }
    @memset(g_fs_scratch[0..g.bps], 0);
    g_fs_scratch[0] = entry_bitmap;
    priv_wr32(&g_fs_scratch[de_first_clus], fmt_first_clus);
    priv_wr32(&g_fs_scratch[de_data_len], g.bitmap_bytes);
    g_fs_scratch[de_second] = entry_upcase;
    priv_wr32(&g_fs_scratch[de_second + de_upc_csum], upcase_csum);
    priv_wr32(&g_fs_scratch[de_second + de_first_clus], g.upcase_cluster);
    priv_wr32(&g_fs_scratch[de_second + de_data_len], fmt_upc_std_bytes);
    g_fs_scratch[de_third] = entry_label;
    const n = internal_exfat_label_utf16(&g_fs_scratch[de_third + de_lbl_name], label);
    g_fs_scratch[de_third + de_lbl_cnt] = @intCast(n);
    return backend[0].write_block.?(backend[0].ctx, g.part_lba +% lba, 1, &g_fs_scratch);
}

/// Encode an LBA as the three-byte CHS field an MBR partition entry carries,
/// using the 255-head / 63-sector geometry; past the 1024-cylinder CHS space
/// the universal 0xFE/0xFF/0xFF "beyond CHS, use LBA" sentinel goes in.
pub fn internal_exfat_lba_to_chs(lba: u32, out3: *[3]u8) void {
    if (lba >= mbr_chs_max) {
        out3[0] = mbr_chs_ovf_h;
        out3[1] = mbr_chs_ovf_m;
        out3[2] = mbr_chs_ovf_l;
        return;
    }
    const cyl = lba / (mbr_chs_heads * mbr_chs_spt);
    const head = (lba / mbr_chs_spt) % mbr_chs_heads;
    const sec = (lba % mbr_chs_spt) + 1;
    out3[0] = @intCast(head);
    out3[1] = @intCast((sec & mbr_chs_sec_mask) | ((cyl & mbr_chs_cyl_hi_mask) >> mbr_chs_cyl_hi_shift));
    out3[2] = @intCast(cyl & byte_mask);
}

/// Write the MBR at LBA 0: a disk signature, one non-bootable type-0x07
/// partition entry describing the exFAT partition, and the 0x55AA signature.
pub fn internal_exfat_write_mbr(backend: [*c]const c.ra8_fs_backend_t, bps: u32, part_lba: u32, part_sectors: u32) u16 {
    @memset(g_fs_scratch[0..bps], 0);
    priv_wr32(&g_fs_scratch[mbr_disk_sig_off], mbr_disk_sig_base ^ part_sectors);
    g_fs_scratch[mbr_part0_off + mbr_pe_boot] = mbr_boot_none;
    internal_exfat_lba_to_chs(part_lba, g_fs_scratch[mbr_part0_off + mbr_pe_chs_start ..][0..3]);
    g_fs_scratch[mbr_part0_off + mbr_pe_type] = mbr_type_exfat;
    internal_exfat_lba_to_chs(part_lba +% part_sectors -% 1, g_fs_scratch[mbr_part0_off + mbr_pe_chs_end ..][0..3]);
    priv_wr32(&g_fs_scratch[mbr_part0_off + mbr_pe_lba], part_lba);
    priv_wr32(&g_fs_scratch[mbr_part0_off + mbr_pe_nsect], part_sectors);
    priv_wr16(&g_fs_scratch[boot_sig_off], @intCast(c.k_exfat_fmt_boot_sig));
    return backend[0].write_block.?(backend[0].ctx, 0, 1, &g_fs_scratch);
}

/// What the placement step resolves for a device: log2 of the sector size,
/// the partition's first LBA, and its sector count.
pub const Placement = struct {
    bps_shift: u8,
    part_lba: u32,
    part_sectors: u32,
};

/// Size and place the exFAT partition on the device, or refuse: the
/// partition aligns at 1 MiB, a device too small for the minimum volume
/// plus the alignment gap is refused, and so is one whose partition would
/// not fit the MBR's 32-bit fields (past-2-TiB media arrive GPT-mounted).
pub fn internal_exfat_fmt_place(total_sectors: u64, bps: u32) ?Placement {
    var bps_shift: u8 = 0;
    var w = bps;
    while (w > 1) : (w >>= 1) {
        bps_shift += 1;
    }
    const part_lba = fmt_part_align / bps;
    const min_total = @as(u64, part_lba) + (@as(u64, fmt_min_sectors) * sector_min) / bps;
    if (total_sectors < min_total) {
        return null;
    }
    if (total_sectors - part_lba > 0xFFFFFFFF) {
        return null;
    }
    return .{
        .bps_shift = bps_shift,
        .part_lba = part_lba,
        .part_sectors = @intCast(total_sectors - part_lba),
    };
}

/// Format a fresh exFAT volume: place the partition, resolve the geometry,
/// write the MBR, the boot region and its backup, seed the FAT, clear and
/// mark the bitmap, write the up-case table, then lay the root directory.
pub export fn priv_exfat_format(backend: [*c]const c.ra8_fs_backend_t, total_sectors: u64, bps: u32, label: [*c]const u8) callconv(.c) u16 {
    const place = internal_exfat_fmt_place(total_sectors, bps) orelse return not_supported;
    var g = std.mem.zeroes(Geom);
    g.bps = bps;
    g.bps_shift = place.bps_shift;
    var err = internal_exfat_geometry(place.part_sectors, &g);
    if (err != ok) {
        return err;
    }
    g.part_lba = place.part_lba;
    // The bitmap/up-case/root chains are seeded in FAT sector 0 only.
    if (@as(u64, g.root_cluster) * 4 >= bps) {
        return not_supported;
    }
    err = internal_exfat_write_mbr(backend, bps, place.part_lba, place.part_sectors);
    if (err != ok) {
        return err;
    }
    err = internal_exfat_write_boot(backend, &g);
    if (err != ok) {
        return err;
    }
    err = internal_exfat_write_fat(backend, &g);
    if (err != ok) {
        return err;
    }
    err = internal_exfat_write_bitmap(backend, &g);
    if (err != ok) {
        return err;
    }
    const upcase_lba = g.part_lba +% g.heap_offset +% @as(u64, g.upcase_cluster - fmt_first_clus) * g.spc;
    var upcase_csum: u32 = 0;
    err = priv_exfat_write_upcase(&backend[0], upcase_lba, bps, &upcase_csum);
    if (err != ok) {
        return err;
    }
    return internal_exfat_write_root(backend, &g, upcase_csum, label);
}
