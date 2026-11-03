//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Where the free space is: next-free hint, FAT sector cache, FSInfo.
//!
//! Port of `ra8_fs_fat_alloc.c`. Two shapes are load-bearing, as in the C:
//! the allocator state is keyed by mount pointer, one slot per mount slot,
//! and one FAT sector cache is shared by every mount, because the adapter is
//! single-threaded by contract and a second buffer would cost 512 bytes of
//! `.bss` to serve a case that cannot arise. The scan itself stays in
//! `ra8_fs_fat.c`, reached through `fs_c.zig`.
//!
//! Bounded loops: the only loops are over the two allocator slots.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;

pub const ok: u16 = c.k_ra8_ok;

const first_data: u32 = @intCast(c.k_cluster_first_data);
const free_unknown: u32 = @intCast(c.k_fs_free_unknown);
const fsinfo_absent: u32 = @intCast(c.k_fs_fsinfo_absent);
const cache_empty: u64 = @intCast(c.k_fs_cache_empty);
const bitmap_unknown: u64 = @intCast(c.k_fs_bitmap_unknown);
const alloc_slots: usize = @intCast(c.k_fs_alloc_slots);
const sector_max: usize = @intCast(c.k_ra8_fs_sector_max);

comptime {
    std.debug.assert(alloc_slots == @as(usize, @intCast(c.k_ra8_fs_max_mounts)));
}

const off_f32_fsinfo: usize = @intCast(c.k_fmt_off_f32_fsinfo);
const fsi_off_lead: usize = @intCast(c.k_fmt_fsi_off_lead);
const fsi_off_struct: usize = @intCast(c.k_fmt_fsi_off_struct);
const fsi_off_trail: usize = @intCast(c.k_fmt_fsi_off_trail);
const fsi_off_free: usize = @intCast(c.k_fmt_fsi_off_free);
const fsi_off_nxtfree: usize = @intCast(c.k_fmt_fsi_off_nxtfree);
const fsi_lead_sig: u32 = @intCast(c.k_fmt_fsi_lead_sig);
const fsi_struct_sig: u32 = @intCast(c.k_fmt_fsi_struct_sig);
const fsi_trail_sig: u32 = @intCast(c.k_fmt_fsi_trail_sig);

/// One mounted volume's answer to "where is the free space". `owner == null`
/// exactly when the slot is free.
const State = struct {
    owner: ?*const Mount = null,
    next_free: u32 = 0,
    free_count: u32 = 0,
    fsinfo_lba: u32 = 0,
    bitmap_lba: u64 = 0,
    dirty: u8 = 0,
};

var s_alloc: [alloc_slots]State = .{ .{}, .{} };
var s_fat_cache: [sector_max]u8 = @splat(0);
var s_fat_cache_lba: u64 = cache_empty;
var s_fat_cache_owner: ?*const Mount = null;

/// Find the allocator state slot bound to `m`; null means unbound.
fn stateFor(m: ?*const Mount) ?*State {
    var i: usize = 0;
    while (i < alloc_slots) : (i += 1) {
        if (s_alloc[i].owner == m) return &s_alloc[i];
    }
    return null;
}

pub export fn priv_alloc_state_bind(m: ?*const Mount) callconv(.c) void {
    var st = stateFor(m);
    if (st == null) {
        st = stateFor(null);
    }
    if (st == null) {
        return;
    }
    st.?.* = .{
        .owner = m,
        .next_free = first_data,
        .free_count = free_unknown,
        .fsinfo_lba = fsinfo_absent,
        .bitmap_lba = bitmap_unknown,
        .dirty = 0,
    };
}

pub export fn priv_exfat_bitmap_lba(m: ?*const Mount, out_lba: *u64) callconv(.c) u16 {
    // Nested rather than joined, as in the C: a mount with no bound slot is
    // not a mount with a cold cache, and no input can produce the first.
    const st = stateFor(m);
    if (st != null) {
        if (st.?.bitmap_lba != bitmap_unknown) {
            out_lba.* = st.?.bitmap_lba;
            return ok;
        }
    }
    var clus: u32 = 0;
    var len: u32 = 0;
    const e = c.priv_exfat_find_bitmap(m, &clus, &len);
    if (e != ok) {
        return e;
    }
    const lba = c.priv_cluster_to_lba(m, clus);
    if (st != null) {
        st.?.bitmap_lba = lba;
    }
    out_lba.* = lba;
    return ok;
}

pub export fn priv_alloc_state_release(m: ?*const Mount) callconv(.c) void {
    const st = stateFor(m);
    if (st != null) {
        st.?.* = .{};
    }
    if (s_fat_cache_owner == m) {
        s_fat_cache_lba = cache_empty;
        s_fat_cache_owner = null;
    }
}

/// True when `lba` lies inside the first FAT copy of `m`. The subtraction is
/// unsigned, so an LBA below the first FAT copy wraps far above any FAT
/// length and the same test rejects it.
fn lbaIsCacheable(m: *const Mount, lba: u64) bool {
    return (lba -% m.first_fat_lba) < m.fat_size_sectors;
}

pub export fn priv_fat_sector_read(m: ?*const Mount, lba: u64, buf: [*]u8) callconv(.c) u16 {
    if (!lbaIsCacheable(m.?, lba)) {
        return c.priv_read_sector(m, lba, buf);
    }
    if ((s_fat_cache_owner == m) and (s_fat_cache_lba == lba)) {
        c.priv_byte_copy(buf, &s_fat_cache, @intCast(c.priv_bps(m)));
        return ok;
    }
    const err = c.priv_read_sector(m, lba, buf);
    if (err != ok) {
        return err;
    }
    c.priv_byte_copy(&s_fat_cache, buf, @intCast(c.priv_bps(m)));
    s_fat_cache_lba = lba;
    s_fat_cache_owner = m;
    return ok;
}

pub export fn priv_fat_sector_wrote(m: ?*const Mount, buf: [*]const u8, lba: u64) callconv(.c) void {
    if (!lbaIsCacheable(m.?, lba)) {
        return;
    }
    c.priv_byte_copy(&s_fat_cache, buf, @intCast(c.priv_bps(m)));
    s_fat_cache_lba = lba;
    s_fat_cache_owner = m;
}

/// Drop the shared FAT sector cache. Test support and mount teardown: the
/// cache is keyed by mount pointer, so a recycled mount address must not
/// inherit the previous owner's sectors.
pub export fn priv_fat_cache_reset() callconv(.c) void {
    s_fat_cache_lba = cache_empty;
    s_fat_cache_owner = null;
}

pub export fn priv_alloc_hint_get(m: ?*const Mount) callconv(.c) u32 {
    const st = stateFor(m);
    if (st == null) {
        return first_data;
    }
    return st.?.next_free;
}

pub export fn priv_alloc_hint_set(m: ?*const Mount, cluster: u32) callconv(.c) void {
    const st = stateFor(m);
    if (st == null) {
        return;
    }
    st.?.next_free = cluster;
}

pub export fn priv_alloc_hint_lower(m: ?*const Mount, cluster: u32) callconv(.c) void {
    const st = stateFor(m);
    if (st == null) {
        return;
    }
    if (cluster < st.?.next_free) {
        st.?.next_free = cluster;
    }
}

pub export fn priv_free_count_took(m: ?*const Mount, n: u32) callconv(.c) void {
    const st = stateFor(m);
    if (st == null) {
        return;
    }
    st.?.dirty = 1;
    if (st.?.free_count == free_unknown) {
        return;
    }
    st.?.free_count = st.?.free_count -| n;
}

pub export fn priv_free_count_gave(m: ?*const Mount, n: u32) callconv(.c) void {
    const st = stateFor(m);
    if (st == null) {
        return;
    }
    st.?.dirty = 1;
    if (st.?.free_count == free_unknown) {
        return;
    }
    const raised = st.?.free_count +% n;
    st.?.free_count = if (raised > m.?.count_of_clusters) m.?.count_of_clusters else raised;
}

pub export fn priv_free_count_peek(m: ?*const Mount) callconv(.c) u32 {
    const st = stateFor(m);
    if (st == null) {
        return free_unknown;
    }
    return st.?.free_count;
}

pub export fn priv_free_count_cache(m: ?*const Mount, n: u32) callconv(.c) void {
    const st = stateFor(m);
    if (st == null) {
        return;
    }
    st.?.free_count = if (n > m.?.count_of_clusters) m.?.count_of_clusters else n;
}

fn rd16(p: []const u8, off: usize) u32 {
    return std.mem.readInt(u16, p[off..][0..2], .little);
}

fn rd32(p: []const u8, off: usize) u32 {
    return std.mem.readInt(u32, p[off..][0..4], .little);
}

fn wr32(p: []u8, off: usize, v: u32) void {
    std.mem.writeInt(u32, p[off..][0..4], v, .little);
}

/// Report the FSInfo sector number this volume declares, or absent. The
/// declared sector must lie inside the volume's own reserved region.
fn fsinfoLocate(m: *const Mount, out_lba: *u32) u16 {
    const boot = c.priv_sec_walk();
    const err = c.priv_read_sector(m, 0, boot);
    if (err != ok) {
        return err;
    }
    const lba = rd16(boot[0..sector_max], off_f32_fsinfo);
    out_lba.* = fsinfo_absent;
    if (lba == fsinfo_absent) {
        return ok;
    }
    if (lba >= m.reserved_sectors) {
        return ok;
    }
    out_lba.* = lba;
    return ok;
}

/// Check the three FSInfo signatures in a candidate sector (MS FAT spec sec
/// 5). All three must match before any field is believed.
fn fsinfoSignaturesOk(sec: []const u8) bool {
    if (rd32(sec, fsi_off_lead) != fsi_lead_sig) {
        return false;
    }
    if (rd32(sec, fsi_off_struct) != fsi_struct_sig) {
        return false;
    }
    return rd32(sec, fsi_off_trail) == fsi_trail_sig;
}

pub export fn priv_fsinfo_seed(m: ?*const Mount) callconv(.c) u16 {
    const st = stateFor(m);
    if (st == null) {
        return ok;
    }
    if (m.?.type != c.k_ra8_fs_type_fat32) {
        return ok;
    }
    var lba: u32 = fsinfo_absent;
    var err = fsinfoLocate(m.?, &lba);
    if (err != ok) {
        return err;
    }
    if (lba == fsinfo_absent) {
        return ok;
    }
    const sec = c.priv_sec_walk();
    err = c.priv_read_sector(m, lba, sec);
    if (err != ok) {
        return err;
    }
    if (!fsinfoSignaturesOk(sec[0..sector_max])) {
        return ok;
    }
    st.?.fsinfo_lba = lba;
    const nxt = rd32(sec[0..sector_max], fsi_off_nxtfree);
    const free = rd32(sec[0..sector_max], fsi_off_free);
    if (nxt >= first_data) {
        if ((nxt - first_data) < m.?.count_of_clusters) {
            st.?.next_free = nxt;
        }
    }
    if (free <= m.?.count_of_clusters) {
        st.?.free_count = free;
    }
    return ok;
}

pub export fn priv_fsinfo_flush(m: ?*const Mount) callconv(.c) u16 {
    const st = stateFor(m);
    if (st == null) {
        return ok;
    }
    if (st.?.fsinfo_lba == fsinfo_absent) {
        return ok;
    }
    if (st.?.dirty == 0) {
        return ok;
    }
    const sec = c.priv_sec_walk();
    var err = c.priv_read_sector(m, st.?.fsinfo_lba, sec);
    if (err != ok) {
        return err;
    }
    wr32(sec[0..sector_max], fsi_off_free, st.?.free_count);
    wr32(sec[0..sector_max], fsi_off_nxtfree, st.?.next_free);
    err = c.priv_write_sector(m, st.?.fsinfo_lba, sec);
    if (err != ok) {
        return err;
    }
    st.?.dirty = 0;
    return ok;
}
