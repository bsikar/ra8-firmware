//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The four-role sector arena: one static max-sector buffer per role
//! (walk, fat, fat2, io), handed out by the `priv_sec_*` accessors. The
//! role semantics and liveness rules live in `ra8_fs_fat_bytes_internal.h`;
//! storage is static and program-wide, single-threaded like every other
//! ra8_fs module-state holder. Ported out of `ra8_fs_fat_mount.c`
//! (RA8FW-1137); the test suites bind their role buffers straight onto
//! these rows instead of faking the accessors.

const c = @import("fs_c.zig").c;

/// Row indices mirror `ra8_fs_sec_role_t` in ra8_fs_fat_types_internal.h
/// (walk = 0, fat = 1, fat2 = 2, io = 3; `k_fs_sec_roles` = 4).
const role_count: usize = 4;
const sector_max: usize = c.k_ra8_fs_sector_max;

/// One row per role, each `k_ra8_fs_sector_max` wide.
pub var s_sec_arena: [role_count][sector_max]u8 = @splat(@splat(0));

/// Walk-role buffer: directory scans and entry read-modify-write.
pub const walk_sector: *[sector_max]u8 = &s_sec_arena[0];

/// FAT-role buffer: the sector holding the FAT entry under work.
pub const fat_sector: *[sector_max]u8 = &s_sec_arena[1];

/// FAT12-straddle buffer: the sector following the FAT-role one.
pub const fat2_sector: *[sector_max]u8 = &s_sec_arena[2];

/// io-role buffer: leaf data and bitmap sector transfers.
pub const io_sector: *[sector_max]u8 = &s_sec_arena[3];

pub export fn priv_sec_walk() callconv(.c) [*c]u8 {
    return &s_sec_arena[0];
}

pub export fn priv_sec_fat() callconv(.c) [*c]u8 {
    return &s_sec_arena[1];
}

pub export fn priv_sec_fat2() callconv(.c) [*c]u8 {
    return &s_sec_arena[2];
}

pub export fn priv_sec_io() callconv(.c) [*c]u8 {
    return &s_sec_arena[3];
}
