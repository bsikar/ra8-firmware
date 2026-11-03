//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! FAT entry access: read or write one FAT entry for any of the four flavours,
//! plus the cluster-to-LBA mapping.
//!
//! Port of the FAT entry layer of `ra8_fs_fat.c`, which this slice empties and
//! removes. MS FAT spec v1.03, Dec 6 2000; NASA Rules 2/3/5/7: one static
//! scratch sector (two when a FAT12 entry straddles a sector boundary), zero
//! malloc, every loop bounded by the mount geometry.
//!
//! Sector I/O goes through the primitive module; the FAT sector cache stays in
//! the allocator module and is reached over the same C ABI every other module
//! uses. The two scratch sectors below carry the old arena roles locally.

const std = @import("std");
const c = @import("fs_c.zig").c;
const prim = @import("ra8_fs_fat_prim_abi.zig");
const core = @import("ra8_fs_fat_core_abi.zig");
/// Scratch sectors, the same fixed-role discipline the C arena uses: one
/// buffer for the entry sector, a second when a FAT12 entry straddles two
/// sectors. Static because 4 KiB frames do not belong on firmware stacks.
var scratch_sector = std.mem.zeroes([c.k_ra8_fs_sector_max]u8);
var scratch_sector2 = std.mem.zeroes([c.k_ra8_fs_sector_max]u8);

const alloc = @import("ra8_fs_alloc_abi.zig");

pub const Mount = c.ra8_fs_mount_t;

pub const ok: u16 = c.k_ra8_ok;
const first_data: u32 = c.k_cluster_first_data;
const fat12_value_mask: u16 = c.k_fat12_value_mask;
const fat12_low_nibble_mask: u16 = c.k_fat12_low_nibble_mask;
const fat12_high_nibble_mask: u16 = c.k_fat12_high_nibble_mask;
const fat32_mask: u32 = c.k_cluster_mask_fat32;
const word_mask: u16 = c.k_word_mask;

fn fatSectorRead(m: [*c]const Mount, lba: u64, buf: [*c]u8) u16 {
    return alloc.priv_fat_sector_read(@ptrCast(m), lba, @ptrCast(buf));
}

fn fatSectorWrote(m: [*c]const Mount, buf: [*c]const u8, lba: u64) void {
    alloc.priv_fat_sector_wrote(@ptrCast(m), @ptrCast(buf), lba);
}

/// Byte offset of a cluster's FAT entry inside the FAT region. exFAT and FAT32
/// entries are 4 bytes; on an exFAT volume near the 2^32-cluster ceiling the
/// product exceeds 32 bits, which is why this returns 64 bits.
fn fatEntryOffset(m: [*c]const Mount, cluster: u32) u64 {
    const clus: u64 = cluster;
    if (m.*.type == c.k_ra8_fs_type_fat12) {
        return clus + clus / 2;
    }
    if (m.*.type == c.k_ra8_fs_type_fat16) {
        return clus * 2;
    }
    return clus * 4;
}

/// Read one FAT entry. MS FAT spec sec 4.1: a FAT12 entry is 12 bits that may
/// straddle a sector boundary, so the second byte is read from the next
/// sector; FAT32's top four bits are reserved and masked off, while exFAT
/// entries are full 32-bit values, because masking there would fold the
/// end-of-chain marker 0xFFFFFFFF down to 0x0FFFFFFF.
pub export fn priv_fat_get(m: [*c]const Mount, cluster: u32, out_value: [*c]u32) callconv(.c) u16 {
    const bps64: u64 = prim.priv_bps(m);
    const fat_offset = fatEntryOffset(m, cluster);
    const sec_num: u64 = m.*.first_fat_lba + fat_offset / bps64;
    const sec_off: u32 = @intCast(fat_offset % bps64);

    const buf: [*c]u8 = &scratch_sector;
    const err = fatSectorRead(m, sec_num, buf);
    if (err != ok) {
        return err;
    }

    var v: u32 = 0;
    if (m.*.type == c.k_ra8_fs_type_fat12) {
        const b0: u8 = buf[sec_off];
        var b1: u8 = 0;
        if (sec_off + 1 < bps64) {
            b1 = buf[sec_off + 1];
        } else {
            const buf2: [*c]u8 = &scratch_sector2;
            const err2 = fatSectorRead(m, sec_num + 1, buf2);
            if (err2 != ok) {
                return err2;
            }
            b1 = buf2[0];
        }
        const raw: u16 = @as(u16, b0) | (@as(u16, b1) << 8);
        if ((cluster & 1) != 0) {
            v = @as(u32, raw) >> 4;
        } else {
            v = @as(u32, raw) & fat12_value_mask;
        }
    } else if (m.*.type == c.k_ra8_fs_type_fat16) {
        v = prim.priv_rd16(buf + sec_off);
    } else if (m.*.type == c.k_ra8_fs_type_exfat) {
        v = prim.priv_rd32(buf + sec_off);
    } else {
        v = prim.priv_rd32(buf + sec_off) & fat32_mask;
    }

    out_value.* = v;
    return ok;
}

/// Write-back half of the FAT12 setter: commit one sector, or both when the
/// entry straddles, and refresh the cache so a later read of either is served
/// from memory rather than the device.
fn fat12Store(m: [*c]const Mount, buf: [*c]u8, buf2: [*c]u8, sec_num: u64, straddle: bool) u16 {
    var err = prim.priv_write_sector(m, sec_num, buf);
    if (err != ok) {
        return err;
    }
    fatSectorWrote(m, buf, sec_num);
    if (!straddle) {
        return ok;
    }
    err = prim.priv_write_sector(m, sec_num + 1, buf2);
    if (err != ok) {
        return err;
    }
    fatSectorWrote(m, buf2, sec_num + 1);
    return ok;
}

/// Write a FAT12 entry, handling sector-straddling 12-bit packing.
fn fat12SetOne(m: [*c]const Mount, sec_num: u64, sec_off: u32, cluster: u32, value: u32) u16 {
    const bps = prim.priv_bps(m);
    const buf: [*c]u8 = &scratch_sector;
    const buf2: [*c]u8 = &scratch_sector2;
    var straddle = false;
    var err = fatSectorRead(m, sec_num, buf);
    if (err != ok) {
        return err;
    }
    const b0: u8 = buf[sec_off];
    var b1: u8 = 0;
    if (sec_off + 1 < bps) {
        b1 = buf[sec_off + 1];
    } else {
        err = fatSectorRead(m, sec_num + 1, buf2);
        if (err != ok) {
            return err;
        }
        b1 = buf2[0];
        straddle = true;
    }
    var raw: u16 = @as(u16, b0) | (@as(u16, b1) << 8);
    if ((cluster & 1) != 0) {
        raw = (raw & fat12_low_nibble_mask) | (@as(u16, @intCast(value & fat12_value_mask)) << 4);
    } else {
        raw = (raw & fat12_high_nibble_mask) | @as(u16, @truncate(value & fat12_value_mask));
    }
    buf[sec_off] = @truncate(raw);
    if (!straddle) {
        buf[sec_off + 1] = @truncate(raw >> 8);
    } else {
        buf2[0] = @truncate(raw >> 8);
    }
    return fat12Store(m, buf, buf2, sec_num, straddle);
}

/// Write a FAT16 entry: a plain 16-bit little-endian field.
fn fat16SetOne(m: [*c]const Mount, sec_num: u64, sec_off: u32, value: u32) u16 {
    const buf: [*c]u8 = &scratch_sector;
    const err = fatSectorRead(m, sec_num, buf);
    if (err != ok) {
        return err;
    }
    prim.priv_wr16(buf + sec_off, @truncate(value & word_mask));
    const werr = prim.priv_write_sector(m, sec_num, buf);
    if (werr != ok) {
        return werr;
    }
    fatSectorWrote(m, buf, sec_num);
    return ok;
}

/// Write a FAT32 entry: the top four bits are reserved and preserved.
fn fat32SetOne(m: [*c]const Mount, sec_num: u64, sec_off: u32, value: u32) u16 {
    const buf: [*c]u8 = &scratch_sector;
    const err = fatSectorRead(m, sec_num, buf);
    if (err != ok) {
        return err;
    }
    const prev = prim.priv_rd32(buf + sec_off) & ~fat32_mask;
    core.priv_wr32(buf + sec_off, (value & fat32_mask) | prev);
    const werr = prim.priv_write_sector(m, sec_num, buf);
    if (werr != ok) {
        return werr;
    }
    fatSectorWrote(m, buf, sec_num);
    return ok;
}

/// Write an exFAT FAT entry, all 32 bits, nothing reserved. exFAT has no
/// reserved high nibble (spec sec 4.1), so the value goes down whole; routing
/// exFAT through the FAT32 setter would mask the end-of-chain marker to
/// 0x0FFFFFFF, which a host fsck reads as a chain pointing past the volume.
fn exfatFatSetOne(m: [*c]const Mount, sec_num: u64, sec_off: u32, value: u32) u16 {
    const buf: [*c]u8 = &scratch_sector;
    const err = fatSectorRead(m, sec_num, buf);
    if (err != ok) {
        return err;
    }
    core.priv_wr32(buf + sec_off, value);
    const werr = prim.priv_write_sector(m, sec_num, buf);
    if (werr != ok) {
        return werr;
    }
    fatSectorWrote(m, buf, sec_num);
    return ok;
}

/// Write one FAT entry into every FAT copy on the volume.
pub export fn priv_fat_set(m: [*c]const Mount, cluster: u32, value: u32) callconv(.c) u16 {
    const bps64: u64 = prim.priv_bps(m);
    const fat_offset = fatEntryOffset(m, cluster);
    var i: u32 = 0;
    while (i < m.*.num_fats) : (i += 1) {
        const fat_base: u64 = m.*.first_fat_lba + @as(u64, i) * m.*.fat_size_sectors;
        const sec_num: u64 = fat_base + fat_offset / bps64;
        const sec_off: u32 = @intCast(fat_offset % bps64);
        const err = if (m.*.type == c.k_ra8_fs_type_fat12)
            fat12SetOne(m, sec_num, sec_off, cluster, value)
        else if (m.*.type == c.k_ra8_fs_type_fat16)
            fat16SetOne(m, sec_num, sec_off, value)
        else if (m.*.type == c.k_ra8_fs_type_exfat)
            exfatFatSetOne(m, sec_num, sec_off, value)
        else
            fat32SetOne(m, sec_num, sec_off, value);
        if (err != ok) {
            return err;
        }
    }
    return ok;
}

/// LBA of the first sector of a cluster. Cluster numbering starts at
/// `k_cluster_first_data` (= 2), so cluster 2 is the first data sector.
pub export fn priv_cluster_to_lba(m: [*c]const Mount, cluster: u32) callconv(.c) u64 {
    // The C computes this in uint64 and wraps: cluster < 2 wraps the
    // subtraction and a huge product wraps the multiply. Match it.
    return m.*.first_data_lba +% ((@as(u64, cluster) -% first_data) *% m.*.sectors_per_cluster);
}
