//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The primitive layer: little-endian field access, the mount-geometry reads,
//! the byte fill/copy helpers, the end-of-chain classifier, and the sector
//! read/write wrappers that forward to the mount's block backend. Every other
//! unit in the adapter builds on these; nothing here allocates.
//!
//! Ported from `ra8_fs_fat.c` (RA8FW-1097): the readers, the 16-bit writer,
//! the byte fill/copy, the geometry reads, the backend wrappers, and the
//! end-of-chain check now come from Zig. The FAT entry get/set layer and its
//! FAT12 straddle helpers stay in C until the test fakes run on a real block
//! backend (the next slice).

const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;

/// Read a little-endian 16-bit field.
pub export fn priv_rd16(p: [*c]const u8) callconv(.c) u16 {
    return @as(u16, p[0]) | (@as(u16, p[1]) << 8);
}

/// Read a little-endian 32-bit field.
pub export fn priv_rd32(p: [*c]const u8) callconv(.c) u32 {
    return @as(u32, p[0]) | (@as(u32, p[1]) << 8) | (@as(u32, p[2]) << 16) | (@as(u32, p[3]) << 24);
}

/// Read a little-endian 64-bit field.
pub export fn priv_rd64(p: [*c]const u8) callconv(.c) u64 {
    return @as(u64, priv_rd32(p)) | (@as(u64, priv_rd32(p + 4)) << 32);
}

/// Write a little-endian 16-bit field.
pub export fn priv_wr16(p: [*c]u8, v: u16) callconv(.c) void {
    p[0] = @truncate(v);
    p[1] = @truncate(v >> 8);
}

/// Bytes per sector, straight from the mount geometry.
pub export fn priv_bps(m: [*c]const Mount) callconv(.c) u32 {
    return @as(u32, m[0].bytes_per_sector);
}

/// Bytes per cluster: sectors per cluster times bytes per sector.
pub export fn priv_cluster_bytes(m: [*c]const Mount) callconv(.c) u32 {
    return @as(u32, m[0].sectors_per_cluster) * @as(u32, m[0].bytes_per_sector);
}

/// Directory entries per sector: 32 bytes per MS FAT directory entry.
pub export fn priv_dir_eps(m: [*c]const Mount) callconv(.c) u32 {
    return @as(u32, m[0].bytes_per_sector) / c.k_ra8_fs_dir_entry_bytes;
}

/// Fill `n` bytes with `value`.
pub export fn priv_byte_fill(dst: [*c]u8, value: u8, n: u32) callconv(.c) void {
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        dst[i] = value;
    }
}

/// Copy `n` bytes from `src` to `dst`.
pub export fn priv_byte_copy(dst: [*c]u8, src: [*c]const u8, n: u32) callconv(.c) void {
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        dst[i] = src[i];
    }
}

/// Read one sector through the mount's block backend, adding the partition
/// base LBA so callers work in volume-relative sector numbers.
pub export fn priv_read_sector(m: [*c]const Mount, lba: u64, buf: [*c]u8) callconv(.c) u16 {
    return m[0].backend.read_block.?(m[0].backend.ctx, lba + m[0].partition_base_lba, 1, buf);
}

/// Write one sector through the mount's block backend, adding the partition
/// base LBA so callers work in volume-relative sector numbers.
pub export fn priv_write_sector(m: [*c]const Mount, lba: u64, buf: [*c]const u8) callconv(.c) u16 {
    return m[0].backend.write_block.?(m[0].backend.ctx, lba + m[0].partition_base_lba, 1, buf);
}

/// Whether `value` reads as an end-of-chain marker on this FAT type. The
/// thresholds come from the MS FAT spec sec 4.1/4.2 and the exFAT spec.
pub export fn priv_is_eoc(m: [*c]const Mount, value: u32) callconv(.c) u8 {
    if (m[0].type == c.k_ra8_fs_type_fat12) return if (value >= c.k_cluster_eoc_min_fat12) 1 else 0;
    if (m[0].type == c.k_ra8_fs_type_fat16) return if (value >= c.k_cluster_eoc_min_fat16) 1 else 0;
    if (m[0].type == c.k_ra8_fs_type_exfat) return if (value >= c.k_cluster_eoc_min_exfat) 1 else 0;
    return if (value >= c.k_cluster_eoc_min_fat32) 1 else 0;
}
