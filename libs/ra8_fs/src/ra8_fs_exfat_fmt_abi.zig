//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT format (mkfs), geometry and boot half: the running boot checksum,
//! the cluster-size tier lookup, the volume geometry resolver, the Main Boot
//! Sector builder, and the writer that lands boot sectors 0-11 and their
//! backups. Ported from `ra8_fs_fat_exfat_fmt.c`, which keeps the write-FAT,
//! bitmap, root, and MBR half until RA8FW slices it too.

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
