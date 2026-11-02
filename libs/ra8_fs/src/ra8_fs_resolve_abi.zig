//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! FAT path resolution: parent and directory lookup.
//!
//! Port of `ra8_fs_fat_resolve.c` (itself split out of the old
//! `ra8_fs_fat_file.c`). `priv_resolve_parent` splits a caller path into a
//! parent directory location plus a leaf name; `priv_resolve_dir` resolves a
//! path to an existing directory. Both descend component by component: each
//! component is looked up, required to carry the directory attribute, and
//! turned into the next location through its first cluster.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;
pub const DirLoc = c.dir_loc_t;

const ok: u16 = c.k_ra8_ok;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_protocol_error: u16 = c.k_ra8_err_protocol_error;

const entry_bytes: usize = @intCast(c.k_ra8_fs_dir_entry_bytes);
const off_attr: usize = @intCast(c.k_dir_off_attr);
const first_data: u32 = @intCast(c.k_cluster_first_data);
const attr_directory: u8 = @intCast(c.k_ra8_fs_attr_directory);
const lfn_cap: u32 = @intCast(c.k_lfn_utf8_cap);

/// Max nested directory components per path (was the file-local enum in the
/// pre-port `ra8_fs_fat_file.c`; the resolver owns the bound now).
const path_max_depth: u32 = 32;

extern fn priv_dir_lookup_any(m: *const Mount, loc: *const DirLoc, leaf: [*:0]const u8, out_lba: *u64, out_off: *u32, out_entry: [*]u8) u16;
extern fn priv_entry_first_cluster(entry: [*]const u8) u32;

/// Descend into the named directory component within `cur`.
///
/// Packs the `len`-byte component to a NUL-terminated name, looks it up in
/// `cur`, requires the matched entry to carry the directory attribute, and
/// returns its first cluster as a subdirectory location.
///
/// The component-length bound is a byte bound against `lfn_cap`: the unit
/// limit lives in `priv_name_classify`, and bounding bytes by the unit cap
/// would refuse a perfectly storable name three characters into a Cyrillic
/// directory.
fn enterSubdir(m: *const Mount, cur: *const DirLoc, comp: [*:0]const u8, len: u32, out: *DirLoc) u16 {
    if (len >= lfn_cap) {
        return err_invalid_arg;
    }
    // Sized for a long name, not an 8.3 one: once `mkdir` can create
    // `/Reading List`, every path THROUGH it has to resolve as well, and a
    // 13-byte buffer would have refused the component before the lookup.
    var namebuf: [lfn_cap]u8 = @splat(0);
    @memcpy(namebuf[0..len], comp[0..len]);
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [entry_bytes]u8 = @splat(0);
    const err = priv_dir_lookup_any(m, cur, @ptrCast(&namebuf), &lba, &off, &entry);
    if (err != ok) {
        return err;
    }
    if ((entry[off_attr] & attr_directory) == 0) {
        return err_invalid_arg;
    }
    const cl = priv_entry_first_cluster(&entry);
    if (cl < first_data) {
        return err_protocol_error;
    }
    out.is_root = 0;
    out.cluster = cl;
    return ok;
}

pub export fn priv_resolve_parent(m: *const Mount, path: [*:0]const u8, out_parent: *DirLoc, out_leaf: *[*:0]const u8) callconv(.c) u16 {
    var i: usize = 0;
    while (path[i] == '/') i += 1;
    var cur = std.mem.zeroes(DirLoc);
    cur.is_root = 1;
    var depth: u32 = 0;
    while (depth < path_max_depth) : (depth += 1) {
        var elen: usize = 0;
        while (path[i + elen] != 0 and path[i + elen] != '/') elen += 1;
        if (path[i + elen] == 0) {
            out_parent.* = cur;
            out_leaf.* = path + i;
            return ok;
        }
        var next = std.mem.zeroes(DirLoc);
        const err = enterSubdir(m, &cur, path + i, @intCast(elen), &next);
        if (err != ok) {
            return err;
        }
        cur = next;
        i += elen;
        while (path[i] == '/') i += 1;
    }
    return err_invalid_arg; // deeper than path_max_depth
}

pub export fn priv_resolve_dir(m: *const Mount, path: [*:0]const u8, out: *DirLoc) callconv(.c) u16 {
    var i: usize = 0;
    while (path[i] == '/') i += 1;
    if (path[i] == 0) {
        out.is_root = 1;
        out.cluster = 0;
        return ok;
    }
    var parent = std.mem.zeroes(DirLoc);
    var leaf: [*:0]const u8 = undefined;
    const err = priv_resolve_parent(m, path, &parent, &leaf);
    if (err != ok) {
        return err;
    }
    var len: u32 = 0;
    while (leaf[len] != 0) len += 1;
    if (len == 0) {
        out.* = parent; // trailing slash: the parent is the directory
        return ok;
    }
    return enterSubdir(m, &parent, leaf, len, out);
}
