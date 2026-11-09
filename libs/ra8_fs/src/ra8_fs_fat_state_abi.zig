//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared FAT/exFAT module state: the one scratch buffer, the read-only
//! zero sector, and the mount/file tables with their slot allocators.
//! Ported out of `ra8_fs_fat_mount.c` (RA8FW-1143); nothing consumes
//! these from C any more. The four-role sector arena and its `priv_sec_*`
//! accessors moved to `ra8_fs_fat_bytes_abi.zig` (RA8FW-1137), and the
//! test suites bind their role buffers onto its rows.

const std = @import("std");
const c = @import("fs_c.zig").c;

/// Single max-sector scratch buffer reused across all I/O. Shared by every
/// FAT/exFAT translation unit; sized to `k_ra8_fs_sector_max` so a 4Kn
/// medium fits without allocation.
pub export var g_fs_scratch: [c.k_ra8_fs_sector_max]u8 = @splat(0);

/// One whole sector of zero bytes, in read-only storage. The shared source
/// for every zero-fill write (fresh directory clusters, cluster tails).
pub export const k_zero_sector: [c.k_ra8_fs_sector_max]u8 = @splat(0);

/// Mount table -- max `k_ra8_fs_max_mounts` simultaneous volumes.
/// Static because firmware does not allocate; the adapter is
/// single-threaded by contract and callers serialise mount/unmount.
var s_mounts: [c.k_ra8_fs_max_mounts]c.ra8_fs_mount_t =
    std.mem.zeroes([c.k_ra8_fs_max_mounts]c.ra8_fs_mount_t);

/// File handle table -- max `k_ra8_fs_max_files` open at once.
pub var s_files: [c.k_ra8_fs_max_files]c.ra8_fs_file_t =
    std.mem.zeroes([c.k_ra8_fs_max_files]c.ra8_fs_file_t);

/// Allocate a free entry from the mount table; returns NULL if full.
///
/// Linear scan of `s_mounts` for an entry with `in_use == 0`. The caller
/// serialises mount/unmount operations; not thread-safe. The returned
/// pointer remains valid for the program lifetime.
pub export fn internal_alloc_mount_slot() callconv(.c) [*c]c.ra8_fs_mount_t {
    for (&s_mounts) |*m| {
        if (m.in_use == 0) return m;
    }
    return null;
}

/// Allocate a free entry from the file handle table; returns NULL if full.
/// Linear scan of `s_files` for an entry with `in_use == 0`. The caller
/// sets `in_use` on the slot it takes; not thread-safe.
pub export fn priv_alloc_file_slot() callconv(.c) [*c]c.ra8_fs_file_t {
    for (&s_files) |*f| {
        if (f.in_use == 0) return f;
    }
    return null;
}

/// Test surface: clear every file slot, as the zero-initialised table
/// starts out.
pub fn filesReset() void {
    for (&s_files) |*f| f.* = std.mem.zeroes(c.ra8_fs_file_t);
}

/// Test surface: mark every file slot busy, standing in for a full table.
pub fn filesFill() void {
    for (&s_files) |*f| f.in_use = 1;
}
