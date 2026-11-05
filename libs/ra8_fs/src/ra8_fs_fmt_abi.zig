//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! FAT12/16/32 formatter (mkfs) for the `ra8_fs` adapter: geometry selection,
//! BPB/FSInfo construction, FAT seeding, and the region-clear helpers shared
//! with the exFAT formatter. Ported whole from `ra8_fs_fat_fmt.c`
//! (RA8FW-1121); the C file is deleted.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const ok: u16 = c.k_ra8_ok;
pub const err_invalid_size: u16 = c.k_ra8_err_invalid_size;

// Geometry + layout constants (ra8_fs_fat_types_internal.h, inc/ra8_fs_types.h).
const num_fats: u32 = @intCast(c.k_fmt_num_fats);
const resv_f16: u32 = @intCast(c.k_fmt_resv_f16);
const resv_f32: u32 = @intCast(c.k_fmt_resv_f32);
const spc_max: u32 = @intCast(c.k_fmt_spc_max);
const f32_entry_bytes: u32 = @intCast(c.k_fmt_fat32_entry_bytes);
const f16_entry_bytes: u32 = @intCast(c.k_fmt_fat16_entry_bytes);
const fat12_max: u32 = @intCast(c.k_cluster_count_fat12_max);
const fat16_max: u32 = @intCast(c.k_cluster_count_fat16_max);
const f32_clus_cap: u64 = @intCast(c.k_fmt_fat32_clus_cap);
const thr_260m: u64 = @intCast(c.k_fmt_f32_thr_260m);
const thr_8g: u64 = @intCast(c.k_fmt_f32_thr_8g);
const thr_16g: u64 = @intCast(c.k_fmt_f32_thr_16g);
const thr_32g: u64 = @intCast(c.k_fmt_f32_thr_32g);
const clus_512b: u5 = @intCast(c.k_fmt_f32_clus_512b);
const clus_4k: u5 = @intCast(c.k_fmt_f32_clus_4k);
const clus_8k: u5 = @intCast(c.k_fmt_f32_clus_8k);
const clus_16k: u5 = @intCast(c.k_fmt_f32_clus_16k);
const clus_32k: u5 = @intCast(c.k_fmt_f32_clus_32k);
const label_len: usize = @intCast(c.k_fmt_label_len);
const name_len: usize = @intCast(c.k_filename_base_len);
const sector_min: u32 = @intCast(c.k_ra8_fs_sector_min);
const word_mask: u64 = @intCast(c.k_word_mask);
const byte_mask: u8 = @intCast(c.k_byte_mask);

// Boot-sector offsets and constants.
const off_jmp0: usize = @intCast(c.k_fmt_off_jmp0);
const off_jmp1: usize = @intCast(c.k_fmt_off_jmp1);
const off_jmp2: usize = @intCast(c.k_fmt_off_jmp2);
const off_oem: usize = @intCast(c.k_fmt_off_oem);
const off_media: usize = @intCast(c.k_fmt_off_media);
const off_trk: usize = @intCast(c.k_fmt_off_sec_per_trk);
const off_heads: usize = @intCast(c.k_fmt_off_num_heads);
const off_f16_drvnum: usize = @intCast(c.k_fmt_off_f16_drvnum);
const off_f16_bootsig: usize = @intCast(c.k_fmt_off_f16_bootsig);
const off_f16_volid: usize = @intCast(c.k_fmt_off_f16_volid);
const off_f16_label: usize = @intCast(c.k_fmt_off_f16_label);
const off_f16_fstype: usize = @intCast(c.k_fmt_off_f16_fstype);
const off_f32_fsinfo: usize = @intCast(c.k_fmt_off_f32_fsinfo);
const off_f32_bkboot: usize = @intCast(c.k_fmt_off_f32_bkboot);
const off_f32_drvnum: usize = @intCast(c.k_fmt_off_f32_drvnum);
const off_f32_bootsig: usize = @intCast(c.k_fmt_off_f32_bootsig);
const off_f32_volid: usize = @intCast(c.k_fmt_off_f32_volid);
const off_f32_label: usize = @intCast(c.k_fmt_off_f32_label);
const off_f32_fstype: usize = @intCast(c.k_fmt_off_f32_fstype);
const bps_off: usize = @intCast(c.k_bpb_off_bytes_per_sec);
const spc_off: usize = @intCast(c.k_bpb_off_sec_per_clus);
const rsvd_off: usize = @intCast(c.k_bpb_off_rsvd_sec_cnt);
const nfats_off: usize = @intCast(c.k_bpb_off_num_fats);
const rootent_off: usize = @intCast(c.k_bpb_off_root_ent_cnt);
const tot16_off: usize = @intCast(c.k_bpb_off_tot_sec_16);
const fatsz16_off: usize = @intCast(c.k_bpb_off_fat_sz_16);
const tot32_off: usize = @intCast(c.k_bpb_off_tot_sec_32);
const fatsz32_off: usize = @intCast(c.k_bpb_off_fat_sz_32);
const rootclus_off: usize = @intCast(c.k_bpb_off_root_clus);
const sig_lo_off: usize = @intCast(c.k_bpb_off_signature_lo);
const sig_hi_off: usize = @intCast(c.k_bpb_off_signature_hi);
const sig_lo: u8 = @intCast(c.k_bpb_sig_lo);
const sig_hi: u8 = @intCast(c.k_bpb_sig_hi);
const media: u32 = @intCast(c.k_fmt_media_fixed);
const media_byte: u8 = @intCast(c.k_fmt_media_fixed);
const drvnum_hd: u8 = @intCast(c.k_fmt_drvnum_hd);
const ext_bootsig: u8 = @intCast(c.k_fmt_ext_bootsig);
const sec_per_trk: u16 = @intCast(c.k_fmt_sec_per_trk);
const num_heads: u16 = @intCast(c.k_fmt_num_heads);
const root_clus_f32: u32 = @intCast(c.k_fmt_root_clus_f32);
const fsinfo_lba: u64 = @intCast(c.k_fmt_fsinfo_sector);
const bkboot_lba: u64 = @intCast(c.k_fmt_bkboot_sector);
const volid_base: u32 = @intCast(c.k_fmt_volid_base);
const jmp0: u8 = @intCast(c.k_fmt_jmp_byte0);
const jmp1: u8 = @intCast(c.k_fmt_jmp_byte1);
const jmp2: u8 = @intCast(c.k_fmt_jmp_byte2);
const fsi_lead_off: usize = @intCast(c.k_fmt_fsi_off_lead);
const fsi_struct_off: usize = @intCast(c.k_fmt_fsi_off_struct);
const fsi_free_off: usize = @intCast(c.k_fmt_fsi_off_free);
const fsi_nxt_off: usize = @intCast(c.k_fmt_fsi_off_nxtfree);
const fsi_trail_off: usize = @intCast(c.k_fmt_fsi_off_trail);
const fsi_lead_sig: u32 = @intCast(c.k_fmt_fsi_lead_sig);
const fsi_struct_sig: u32 = @intCast(c.k_fmt_fsi_struct_sig);
const fsi_trail_sig: u32 = @intCast(c.k_fmt_fsi_trail_sig);
const fsi_unknown: u32 = @intCast(c.k_fmt_fsi_unknown);
const f32_nxt_free: u32 = @intCast(c.k_fmt_fat32_nxt_free);
const eoc_min_fat16: u32 = @intCast(c.k_cluster_eoc_min_fat16);
const eoc_min_fat32: u32 = @intCast(c.k_cluster_eoc_min_fat32);
const eoc_write_fat16: u16 = @intCast(c.k_cluster_eoc_write_fat16);
const eoc_write_fat32: u32 = @intCast(c.k_cluster_eoc_write_fat32);

const Geom = c.ra8_fs_fmt_geom_t;
const Backend = c.ra8_fs_backend_t;

/// All-zero source for the chunked FAT/root wipe (read-only, in flash).
const zero_chunk: [16384]u8 = @splat(0);

pub export fn priv_fmt_reserved_for(vol_type: u8) callconv(.c) u32 {
    return if (vol_type == c.k_ra8_fs_type_fat32) resv_f32 else resv_f16;
}

/// FAT entries per DEVICE sector for the trial cluster size, plus the resulting
/// data-cluster count. Mirrors the mount-side geometry: subtracts the reserved,
/// FAT, and fixed-root regions, then divides the remaining data sectors by `spc`.
fn clustersFor(g: *const Geom, spc: u32, out_fatsz: *u32) u64 {
    const entry_bytes: u32 = if (g.type == c.k_ra8_fs_type_fat32) f32_entry_bytes else f16_entry_bytes;
    const entry_cap: u64 = @divTrunc(g.bytes_per_sector, entry_bytes);
    const overhead: u64 = @as(u64, g.reserved_sectors) + g.root_sectors;
    if (g.total_sectors <= overhead) {
        out_fatsz.* = 0;
        return 0;
    }
    const avail: u64 = g.total_sectors - overhead;
    const denom: u64 = (entry_cap * spc) + num_fats;
    const fatsz: u64 = (avail + denom - 1) / denom;
    out_fatsz.* = @intCast(fatsz);
    const fat_total: u64 = @as(u64, num_fats) * fatsz;
    if (avail <= fat_total) {
        return 0;
    }
    return (avail - fat_total) / spc;
}

/// MS FAT spec sec 3.5 thresholds: FAT12 < 4085, FAT16 4085..65524,
/// FAT32 65525..k_fmt_fat32_clus_cap.
fn countInBand(vol_type: c.ra8_fs_type_t, count: u64) bool {
    if (vol_type == c.k_ra8_fs_type_fat12) {
        return count < fat12_max;
    }
    if (vol_type == c.k_ra8_fs_type_fat16) {
        return (count >= fat12_max) and (count < fat16_max);
    }
    return (count >= fat16_max) and (count <= f32_clus_cap);
}

/// log2 of a power-of-two value. Loop-counted rather than a builtin, so the
/// host tests and every cross toolchain agree byte-for-byte.
fn log2(v: u32) u32 {
    var r: u32 = 0;
    var w = v;
    while (w > 1) : (w >>= 1) {
        r += 1;
    }
    return r;
}

/// Default FAT32 cluster size for a device, per the MS DskSzToSecPerClus
/// table (fatgen103 sec 3.3): the starting cluster size for the auto-sweep.
/// The table's thresholds are 512-byte-sector counts and its picks are cluster
/// BYTES, both converted through the device's real sector size.
fn fat32DefaultSpc(total_sectors: u64, bps: u32) u32 {
    const bps_shift: u5 = @intCast(log2(bps));
    const half: u5 = @intCast(bps_shift - log2(sector_min));
    const total_512: u64 = total_sectors << half;
    var clus_shift: u5 = clus_32k;
    if (total_512 <= thr_260m) {
        clus_shift = clus_512b;
    } else if (total_512 <= thr_8g) {
        clus_shift = clus_4k;
    } else if (total_512 <= thr_16g) {
        clus_shift = clus_8k;
    } else if (total_512 <= thr_32g) {
        clus_shift = clus_16k;
    }
    if (clus_shift <= bps_shift) {
        return 1;
    }
    const up: u5 = @intCast(clus_shift - bps_shift);
    return @as(u32, 1) << up;
}

/// Resolve the on-disk geometry for a format run: sweep sectors-per-cluster
/// (starting at the MS-table default for FAT32, the minimum for FAT12/16)
/// until the cluster count lands in the type's valid band. A size the FAT
/// BPB's 32-bit fields cannot describe fails cleanly (exFAT is the choice
/// there). Caller pre-fills type, totals, sector size, and the reserved /
/// fixed-root spans (reserved via `priv_fmt_reserved_for()`).
pub export fn priv_fmt_choose_geometry(g: *Geom, spc_hint: u32) callconv(.c) u16 {
    if (g.total_sectors > std.math.maxInt(u32)) {
        return err_invalid_size;
    }
    const auto_mode = (spc_hint == 0);
    var spc: u32 = spc_hint;
    if (auto_mode) {
        spc = if (g.type == c.k_ra8_fs_type_fat32)
            fat32DefaultSpc(g.total_sectors, g.bytes_per_sector)
        else
            1;
    }
    var guard: u32 = 0;
    while (guard <= spc_max) : (guard += 1) {
        var fatsz: u32 = 0;
        const count = clustersFor(g, spc, &fatsz);
        if (countInBand(g.type, count)) {
            g.sectors_per_cluster = spc;
            g.fat_size_sectors = fatsz;
            g.count_of_clusters = @intCast(count);
            return ok;
        }
        if (!auto_mode or spc >= spc_max) {
            return err_invalid_size;
        }
        spc *= 2;
    }
    return err_invalid_size;
}

/// Write the shared boot prologue (jump + OEM) into the scratch sector.
fn bootPrologue(sec: []u8) void {
    sec[off_jmp0] = jmp0;
    sec[off_jmp1] = jmp1;
    sec[off_jmp2] = jmp2;
    @memcpy(sec[off_oem..][0..name_len], "MSDOS5.0");
}

/// An unlabelled FAT volume stores the spec sentinel "NO NAME    ", never
/// zeros and never a bare run of spaces: fsck.fat reads a blank BS_VolLab as a
/// corrupt label and strips it on sight. A null or empty label therefore
/// resolves to that sentinel before padding.
pub export fn priv_fmt_label_field(dst: [*c]u8, label: [*c]const u8) callconv(.c) void {
    var eff: []const u8 = "NO NAME";
    if (label != null and label[0] != 0) {
        eff = std.mem.span(label);
    }
    var past_end = false;
    var i: usize = 0;
    while (i < label_len) : (i += 1) {
        if (!past_end and i >= eff.len) {
            past_end = true;
        }
        dst[i] = if (past_end) ' ' else eff[i];
    }
}

/// Write the totals/FAT-size pair into the BPB, choosing 16- vs 32-bit fields.
/// Larger values spill to the 32-bit fields with the 16-bit field left zero
/// (FAT12/16); FAT32 always uses the 32-bit fields.
fn writeTotals(sec: []u8, g: *const Geom) void {
    if (g.type == c.k_ra8_fs_type_fat32) {
        c.priv_wr32(sec[tot32_off..].ptr, @truncate(g.total_sectors));
        c.priv_wr32(sec[fatsz32_off..].ptr, g.fat_size_sectors);
        return;
    }
    if (g.total_sectors <= word_mask) {
        c.priv_wr16(sec[tot16_off..].ptr, @intCast(g.total_sectors));
    } else {
        c.priv_wr32(sec[tot32_off..].ptr, @truncate(g.total_sectors));
    }
    c.priv_wr16(sec[fatsz16_off..].ptr, @intCast(g.fat_size_sectors));
}

/// Build the FAT12/FAT16 boot sector: full BPB, extended boot signature with
/// volume serial + label, the "FAT12   "/"FAT16   " type string, 0x55AA.
fn buildBpbF16(sec: []u8, g: *const Geom, label: [*c]const u8) void {
    bootPrologue(sec);
    c.priv_wr16(sec[bps_off..].ptr, @intCast(g.bytes_per_sector));
    sec[spc_off] = @intCast(g.sectors_per_cluster);
    c.priv_wr16(sec[rsvd_off..].ptr, @intCast(g.reserved_sectors));
    sec[nfats_off] = @intCast(num_fats);
    c.priv_wr16(sec[rootent_off..].ptr, @intCast(g.root_entries));
    sec[off_media] = media_byte;
    writeTotals(sec, g);
    c.priv_wr16(sec[off_trk..].ptr, sec_per_trk);
    c.priv_wr16(sec[off_heads..].ptr, num_heads);
    sec[off_f16_drvnum] = drvnum_hd;
    sec[off_f16_bootsig] = ext_bootsig;
    c.priv_wr32(sec[off_f16_volid..].ptr, volid_base | @as(u32, @truncate(g.total_sectors)));
    priv_fmt_label_field(sec[off_f16_label..].ptr, label);
    const t12 = "FAT12   ";
    const t16 = "FAT16   ";
    @memcpy(sec[off_f16_fstype..][0..name_len], if (g.type == c.k_ra8_fs_type_fat12) t12 else t16);
    sec[sig_lo_off] = sig_lo;
    sec[sig_hi_off] = sig_hi;
}

/// Build the FAT32 boot sector: zero root entries, 32-bit totals + FAT size,
/// root cluster 2, FSInfo + backup-boot locations, signature + label.
fn buildBpbF32(sec: []u8, g: *const Geom, label: [*c]const u8) void {
    bootPrologue(sec);
    c.priv_wr16(sec[bps_off..].ptr, @intCast(g.bytes_per_sector));
    sec[spc_off] = @intCast(g.sectors_per_cluster);
    c.priv_wr16(sec[rsvd_off..].ptr, @intCast(g.reserved_sectors));
    sec[nfats_off] = @intCast(num_fats);
    sec[off_media] = media_byte;
    writeTotals(sec, g);
    c.priv_wr16(sec[off_trk..].ptr, sec_per_trk);
    c.priv_wr16(sec[off_heads..].ptr, num_heads);
    c.priv_wr32(sec[rootclus_off..].ptr, root_clus_f32);
    c.priv_wr16(sec[off_f32_fsinfo..].ptr, @intCast(fsinfo_lba));
    c.priv_wr16(sec[off_f32_bkboot..].ptr, @intCast(bkboot_lba));
    sec[off_f32_drvnum] = drvnum_hd;
    sec[off_f32_bootsig] = ext_bootsig;
    c.priv_wr32(sec[off_f32_volid..].ptr, volid_base | @as(u32, @truncate(g.total_sectors)));
    priv_fmt_label_field(sec[off_f32_label..].ptr, label);
    @memcpy(sec[off_f32_fstype..][0..name_len], "FAT32   ");
    sec[sig_lo_off] = sig_lo;
    sec[sig_hi_off] = sig_hi;
}

/// Zero a run of sectors on the backend in multi-sector chunks, so an SD
/// backend clears the whole region with CMD25 multi-block writes instead of
/// one slow CMD24 per sector (a 32 MB FAT on a 128 GB card drops from minutes
/// to seconds). Bounded by the caller's count; first failed write aborts.
fn zeroRun(backend: *const Backend, lba: u64, count: u64, bps: u32) u16 {
    const chunk_sectors: u32 = @as(u32, zero_chunk.len) / bps;
    var done: u64 = 0;
    while (done < count) {
        var chunk: u32 = chunk_sectors;
        if (chunk > count - done) {
            chunk = @intCast(count - done);
        }
        const err = backend.write_block.?(backend.ctx, lba + done, chunk, &zero_chunk);
        if (err != ok) {
            return err;
        }
        done += chunk;
    }
    return ok;
}

/// Zero a region when the backend cannot guarantee zero-after-erase. Erase the
/// whole span in one go when the card guarantees a zero read-back, else stream
/// zeros in chunks.
pub export fn priv_fmt_clear_region(backend: *const Backend, lba: u64, count: u64, bps: u32) callconv(.c) u16 {
    if (backend.erase_blocks) |erase| {
        if (erase(backend.ctx, lba, count) == ok) {
            return ok;
        }
    }
    return zeroRun(backend, lba, count, bps);
}

/// Seed the reserved FAT entries (and FAT32 root-cluster EOC) per copy:
/// FAT[0] (media descriptor in the low byte, rest 1-bits), FAT[1] (clean
/// end-of-chain), plus FAT[2] on FAT32 marking the root cluster EOC.
fn seedFats(backend: *const Backend, g: *const Geom) u16 {
    const scratch: [*]u8 = &c.g_fs_scratch;
    @memset(scratch[0..g.bytes_per_sector], 0);
    if (g.type == c.k_ra8_fs_type_fat32) {
        c.priv_wr32(scratch, eoc_min_fat32 | media);
        c.priv_wr32(scratch + 4, eoc_write_fat32);
        c.priv_wr32(scratch + 8, eoc_write_fat32);
    } else if (g.type == c.k_ra8_fs_type_fat16) {
        c.priv_wr16(scratch, @intCast(eoc_min_fat16 | media));
        c.priv_wr16(scratch + 2, eoc_write_fat16);
    } else {
        // FAT12: 1.5-byte entries. FAT[0]=0xF<media>, FAT[1]=0xFFF, packed LE.
        scratch[0] = media_byte;
        scratch[1] = byte_mask;
        scratch[2] = byte_mask;
    }
    var f: u32 = 0;
    while (f < num_fats) : (f += 1) {
        const fat_lba: u64 = @as(u64, g.reserved_sectors) + @as(u64, f) * g.fat_size_sectors;
        const err = backend.write_block.?(backend.ctx, fat_lba, 1, scratch);
        if (err != ok) {
            return err;
        }
    }
    return ok;
}

/// Write the FAT32 FSInfo sector (lead/struct/trail signatures plus a
/// best-effort free count + next-free hint) and copy the boot sector to the
/// backup location. No-op for FAT12/16.
fn writeFsinfo(backend: *const Backend, g: *const Geom, boot: [*]const u8) u16 {
    if (g.type != c.k_ra8_fs_type_fat32) {
        return ok;
    }
    const scratch: [*]u8 = &c.g_fs_scratch;
    @memset(scratch[0..g.bytes_per_sector], 0);
    c.priv_wr32(scratch + fsi_lead_off, fsi_lead_sig);
    c.priv_wr32(scratch + fsi_struct_off, fsi_struct_sig);
    const free_count: u32 = if (g.count_of_clusters > 0) g.count_of_clusters - 1 else fsi_unknown;
    c.priv_wr32(scratch + fsi_free_off, free_count);
    c.priv_wr32(scratch + fsi_nxt_off, f32_nxt_free);
    c.priv_wr32(scratch + fsi_trail_off, fsi_trail_sig);
    const err = backend.write_block.?(backend.ctx, fsinfo_lba, 1, scratch);
    if (err != ok) {
        return err;
    }
    return backend.write_block.?(backend.ctx, bkboot_lba, 1, boot);
}

/// A sectors-per-cluster hint is valid when 0 (auto), or a power of two in
/// 1..k_fmt_spc_max.
pub export fn priv_fmt_spc_valid(spc: u8) callconv(.c) bool {
    if (spc == 0) {
        return true;
    }
    if (spc > spc_max) {
        return false;
    }
    const wide: u32 = spc;
    return (wide & (wide - 1)) == 0;
}

/// Lay a fresh volume down: build the boot sector, clear the FAT region plus
/// the (contiguous) root region BEFORE any metadata write (so an erase that
/// rounds into the reserved region cannot clobber what is written below), then
/// write the boot sector, seed each FAT copy's first sector, and lay the
/// FAT32 FSInfo + backup boot.
pub export fn priv_fmt_emit_volume(backend: *const Backend, g: *const Geom, label: [*c]const u8) callconv(.c) u16 {
    const boot: [*]u8 = c.priv_sec_walk();
    @memset(boot[0..g.bytes_per_sector], 0);
    if (g.type == c.k_ra8_fs_type_fat32) {
        buildBpbF32(boot[0..g.bytes_per_sector], g, label);
    } else {
        buildBpbF16(boot[0..g.bytes_per_sector], g, label);
    }
    const fat_total: u64 = @as(u64, num_fats) * g.fat_size_sectors;
    const root_span: u64 = if (g.type == c.k_ra8_fs_type_fat32) g.sectors_per_cluster else g.root_sectors;
    var err = priv_fmt_clear_region(backend, g.reserved_sectors, fat_total + root_span, g.bytes_per_sector);
    if (err != ok) {
        return err;
    }
    err = backend.write_block.?(backend.ctx, 0, 1, boot);
    if (err != ok) {
        return err;
    }
    err = seedFats(backend, g);
    if (err != ok) {
        return err;
    }
    return writeFsinfo(backend, g, boot);
}
