//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Adding and taking away a directory: `mkdir` / `rmdir` over the FAT shape.
//!
//! Port of `ra8_fs_fat_dirmk.c`. The two verbs stay together because they are
//! inverses over the same on-disk shape: `mkdir` allocates a cluster, stamps
//! `.` and `..` into it and links an ATTR_DIRECTORY entry, and `rmdir` undoes
//! exactly that after proving the directory holds nothing but those two dot
//! entries. exFAT has neither dot entries nor a parent back-link, so its half
//! stays in `ra8_fs_fat_exfat_dir.c`, reached through `fs_c.zig`.
//!
//! The emptiness scan skips deleted slots (0xE5), long-name slots (0x0F) and
//! the dot entries, for the same reason the C gives: `ra8_fs_unlink()` clears
//! only the 8.3 entry, so a removed file leaves its whole LFN chain behind,
//! and counting those remnants would make a genuinely empty directory
//! permanently un-removable.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;

pub const ok: u16 = c.k_ra8_ok;

const entry_bytes: usize = @intCast(c.k_ra8_fs_dir_entry_bytes);
const name_field_len: usize = @intCast(c.k_dir_name_field_len);
const off_name: usize = @intCast(c.k_dir_off_name);
const off_attr: usize = @intCast(c.k_dir_off_attr);
const marker_free_perm: u8 = @intCast(c.k_dir_marker_free_perm);
const marker_free_used: u8 = @intCast(c.k_dir_marker_free_used);
const marker_dot: u8 = @intCast(c.k_dir_marker_dot);
const attr_directory: u8 = @intCast(c.k_ra8_fs_attr_directory);
const attr_lfn: u8 = @intCast(c.k_ra8_fs_attr_lfn);
const first_data: u32 = @intCast(c.k_cluster_first_data);
const sector_max: usize = @intCast(c.k_ra8_fs_sector_max);

const zero_sector: [sector_max]u8 = @splat(0);

const scan_more: u8 = 0;
const scan_empty: u8 = 1;
const scan_used: u8 = 2;

/// Pack a "." or ".." dot entry into a 32-byte directory slot: space-padded
/// name of `dots` dots, the directory attribute, `cluster` as the first
/// cluster, size 0. A parent that is the volume root is recorded as cluster 0
/// per the FAT specification.
fn pack_dot_entry(ent: [*]u8, dots: u32, cluster: u32) void {
    for (0..entry_bytes) |i| {
        ent[i] = 0;
    }
    for (0..name_field_len) |i| {
        ent[i] = ' ';
    }
    for (0..dots) |i| {
        ent[i] = '.';
    }
    ent[off_attr] = attr_directory;
    c.priv_entry_set_cluster_size(ent, cluster, 0);
    // "." and ".." are ordinary directory entries to every host, and every
    // mkfs stamps them. Leaving them at zero would put an illegal date inside
    // a directory this firmware had just created.
    c.priv_fat_entry_stamp_create(ent);
}

/// Initialise a freshly allocated directory cluster (".", "..", then zeros).
fn dir_cluster_init(m: ?*const Mount, new_cluster: u32, parent_cluster: u32) u16 {
    const buf: [*]u8 = c.priv_sec_walk();
    c.priv_byte_fill(buf, 0, c.priv_bps(m));
    pack_dot_entry(buf, 1, new_cluster);
    pack_dot_entry(buf + entry_bytes, 2, parent_cluster);
    const base: u64 = c.priv_cluster_to_lba(m, new_cluster);
    var err: u16 = c.priv_write_sector(m, base, buf);
    if (err != ok) {
        return err;
    }
    var s: u32 = 1;
    while (s < m.?.sectors_per_cluster) : (s += 1) {
        err = c.priv_write_sector(m, base + @as(u64, s), &zero_sector);
        if (err != ok) {
            return err;
        }
    }
    return ok;
}

/// Create one FAT directory: resolve the parent, refuse an existing name,
/// reserve a slot, allocate and initialise the directory cluster, then write
/// the parent's entry. Any post-allocation failure frees the new cluster.
fn fat_mkdir(handle: ?*const Mount, path: [*c]const u8) u16 {
    var parent: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var leaf: [*c]const u8 = null;
    const rerr: u16 = c.priv_resolve_parent(handle, path, &parent, &leaf);
    if (rerr != ok) {
        return rerr;
    }
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [entry_bytes]u8 = @splat(0);
    // By long name as well as by 8.3: a second `mkdir("/Reading List")` has to
    // be refused, and the alias probe behind `priv_dir_reserve()` would
    // happily file a second copy as `READIN~2` if it were not.
    if (c.priv_dir_lookup_any(handle, &parent, leaf, &lba, &off, &entry) == ok) {
        return c.k_ra8_err_exists;
    }
    var plan: c.dir_insert_t = std.mem.zeroes(c.dir_insert_t);
    var err: u16 = c.priv_dir_reserve(handle, &parent, leaf, &plan);
    if (err != ok) {
        return err;
    }
    var new_cluster: u32 = 0;
    err = c.priv_alloc_eoc_cluster(handle, &new_cluster);
    if (err != ok) {
        return err;
    }
    const parent_cluster: u32 = if (parent.is_root != 0) 0 else parent.cluster;
    err = dir_cluster_init(handle, new_cluster, parent_cluster);
    if (err != ok) {
        _ = c.priv_free_chain(handle, new_cluster);
        return err;
    }
    var tmpl: [entry_bytes]u8 = @splat(0);
    tmpl[off_attr] = attr_directory;
    c.priv_entry_set_cluster_size(&tmpl, new_cluster, 0);
    // Same reasoning as the file case: a zero FAT date is illegal, and the
    // directory's own "." and ".." were stamped when the cluster was built,
    // so the entry in the parent must agree with them.
    c.priv_fat_entry_stamp_create(&tmpl);
    err = c.priv_dir_commit(handle, &plan, &tmpl, &lba, &off);
    if (err != ok) {
        _ = c.priv_free_chain(handle, new_cluster);
        return err;
    }
    return ok;
}

/// Create a directory: the guarded body of `ra8_fs_mkdir()`.
fn mkdir_locked(handle: ?*const Mount, path: [*c]const u8) u16 {
    if (handle == null) {
        return c.k_ra8_err_null_ptr;
    }
    if (path == null) {
        return c.k_ra8_err_null_ptr;
    }
    if (handle.?.in_use == 0) {
        return c.k_ra8_err_invalid_state;
    }
    if (handle.?.type == c.k_ra8_fs_type_exfat) {
        return c.priv_exfat_mkdir(handle, path);
    }
    return fat_mkdir(handle, path);
}

/// Classify one already-loaded directory sector as empty / occupied / more.
/// A slot does not count as an occupant when it is deleted (0xE5), an
/// attr-0x0F long-name slot, or one of the synthetic "." / ".." dot entries.
/// A live LFN chain is always followed by its own 8.3 entry in the same
/// directory, so nothing real is missed by skipping it.
fn rmdir_scan_sector(m: ?*const Mount, buf: [*]const u8) u8 {
    const eps: u32 = c.priv_dir_eps(m);
    var e: u32 = 0;
    while (e < eps) : (e += 1) {
        const ent = buf + @as(usize, e) * entry_bytes;
        if (ent[off_name] == marker_free_perm) {
            return scan_empty;
        }
        if (ent[off_name] == marker_free_used) {
            continue;
        }
        if (ent[off_attr] == attr_lfn) {
            continue;
        }
        if (ent[off_name] == marker_dot) {
            continue;
        }
        return scan_used;
    }
    return scan_more;
}

/// Decide whether a subdirectory holds any entry other than "." / "..".
/// Walks the cluster chain with the shared `dir_walk_t` iterator; the walk
/// itself fails a chain that revisits a cluster, so the loop cannot run away
/// on a corrupt FAT.
fn dir_is_empty(m: ?*const Mount, cluster: u32, out_empty: *u8) u16 {
    const loc: c.dir_loc_t = .{ .is_root = 0, .cluster = cluster };
    var w: c.dir_walk_t = std.mem.zeroes(c.dir_walk_t);
    c.priv_dir_walk_init_loc(m, &loc, &w);
    var eod: u8 = 0;
    const buf: [*]u8 = c.priv_sec_walk();
    while (eod == 0) {
        var err: u16 = c.priv_read_sector(m, w.cur_lba, buf);
        if (err != ok) {
            return err;
        }
        const verdict: u8 = rmdir_scan_sector(m, buf);
        if (verdict == scan_used) {
            out_empty.* = 0;
            return ok;
        }
        if (verdict == scan_empty) {
            out_empty.* = 1;
            return ok;
        }
        err = c.priv_dir_walk_next_sector(m, &w, &eod);
        if (err != ok) {
            return err;
        }
    }
    out_empty.* = 1; // chain exhausted with no occupant and no 0x00 marker
    return ok;
}

/// Resolve a path to a removable subdirectory's entry and first cluster:
/// refuse the volume root, require ATTR_DIRECTORY and a real first cluster.
fn rmdir_locate(handle: ?*const Mount, path: [*c]const u8, out: *c.dir_target_t, out_cluster: *u32) u16 {
    var leaf: [*c]const u8 = null;
    const rerr: u16 = c.priv_resolve_parent(handle, path, &out.parent, &leaf);
    if (rerr != ok) {
        return rerr;
    }
    if (leaf[0] == 0) {
        return c.k_ra8_err_invalid_arg; // "/" or a trailing slash: the root itself
    }
    const err: u16 = c.priv_dir_lookup_any(handle, &out.parent, leaf, &out.lba, &out.off, &out.entry);
    if (err != ok) {
        return err;
    }
    if ((out.entry[off_attr] & attr_directory) == 0) {
        return c.k_ra8_err_invalid_arg; // a file: ra8_fs_unlink() is the verb for it
    }
    out_cluster.* = c.priv_entry_first_cluster(&out.entry);
    if (out_cluster.* < first_data) {
        return c.k_ra8_err_protocol_error; // a directory always owns a cluster
    }
    return ok;
}

/// Remove one empty FAT directory: locate it, prove it holds nothing but its
/// own "." and ".." links, free its cluster chain, then 0xE5-mark its entry
/// in the parent. The emptiness proof runs before anything is freed, so a
/// refused removal changes nothing on disk.
fn fat_rmdir(handle: ?*const Mount, path: [*c]const u8) u16 {
    var t: c.dir_target_t = std.mem.zeroes(c.dir_target_t);
    var cluster: u32 = 0;
    var err: u16 = rmdir_locate(handle, path, &t, &cluster);
    if (err != ok) {
        return err;
    }
    var empty: u8 = 0;
    err = dir_is_empty(handle, cluster, &empty);
    if (err != ok) {
        return err;
    }
    if (empty == 0) {
        return c.k_ra8_err_not_empty;
    }
    err = c.priv_free_chain(handle, cluster);
    if (err != ok) {
        return err;
    }
    return c.priv_dir_erase_chain(handle, &t.parent, t.lba, t.off, &t.entry[off_name]);
}

/// Remove an empty directory: the guarded body of `ra8_fs_rmdir()`.
fn rmdir_locked(handle: ?*const Mount, path: [*c]const u8) u16 {
    if (handle == null) {
        return c.k_ra8_err_null_ptr;
    }
    if (path == null) {
        return c.k_ra8_err_null_ptr;
    }
    if (handle.?.in_use == 0) {
        return c.k_ra8_err_invalid_state;
    }
    if (handle.?.type == c.k_ra8_fs_type_exfat) {
        return c.priv_exfat_rmdir(handle, path);
    }
    return fat_rmdir(handle, path);
}

pub export fn ra8_fs_mkdir(handle: ?*const Mount, path: [*c]const u8) callconv(.c) u16 {
    c.priv_lock_acquire();
    const err: u16 = mkdir_locked(handle, path);
    c.priv_lock_release();
    return err;
}

pub export fn ra8_fs_rmdir(handle: ?*const Mount, path: [*c]const u8) callconv(.c) u16 {
    c.priv_lock_acquire();
    const err: u16 = rmdir_locked(handle, path);
    c.priv_lock_release();
    return err;
}
