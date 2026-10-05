//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_free_chain`: release a FAT cluster chain back to the volume.
//!
//! Each cluster's link is read before the entry is cleared, the free count
//! is credited and the allocation hint pulled back so the freed space is the
//! next thing handed out. The walk stops at an end-of-chain marker or at the
//! first value outside the data region. The FAT reader and writer, the
//! free-count cache and the hint stay in C, reached through `fs_c.zig`.
//!
//! Bounded loop: a chain can't be longer than `count_of_clusters`, so a
//! longer walk is a cycle and returns `k_ra8_err_protocol_error`.

const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;

pub const ok: u16 = c.k_ra8_ok;
pub const err_protocol: u16 = c.k_ra8_err_protocol_error;

const first_data: u32 = @intCast(c.k_cluster_first_data);
const cluster_free: u32 = @intCast(c.k_cluster_free);

/// True when `clus` names a cluster in the data region.
fn inData(m: *const Mount, clus: u32) bool {
    return clus >= first_data and (clus - first_data) < m.count_of_clusters;
}

/// Free every cluster in the chain that starts at `start`.
pub export fn priv_free_chain(m: *const Mount, start: u32) callconv(.C) u16 {
    var cur = start;
    var guard: u32 = 0;
    while (inData(m, cur)) {
        var next: u32 = 0;
        var err = c.priv_fat_get(m, cur, &next);
        if (err != ok) return err;
        err = c.priv_fat_set(m, cur, cluster_free);
        if (err != ok) return err;
        c.priv_free_count_gave(m, 1);
        c.priv_alloc_hint_lower(m, cur);
        if (c.priv_is_eoc(m, next) != 0) break;
        cur = next;
        guard += 1;
        if (guard > m.count_of_clusters) return err_protocol;
    }
    return ok;
}
