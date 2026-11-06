//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! FAT listdir / unlink / rename directory-entry operations.
//!
//! Port of `ra8_fs_fat_dir.c`. The verbs that read and rewrite entries in a
//! directory that already exists: listing it, deleting a file from it, and
//! renaming one in place, dispatched across FAT and exFAT volumes. `unlink`
//! refuses a directory -- freeing the cluster chain behind one orphans every
//! file inside it; removing a directory is `rmdir`'s job, in
//! `ra8_fs_dirmk_abi.zig`. Every helper the FAT halves call is already Zig:
//! resolution (`ra8_fs_resolve_abi.zig`), the sector walk
//! (`ra8_fs_dir_walk_abi.zig`), LFN reassembly (`ra8_fs_lfn_abi.zig`),
//! lookup, erase and reserve/commit, so this unit is pure orchestration and
//! the exFAT halves are one dispatch away.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;

const ok: u16 = c.k_ra8_ok;
const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_not_found: u16 = c.k_ra8_err_not_found;
const err_access_denied: u16 = c.k_ra8_err_access_denied;
const err_exists: u16 = c.k_ra8_err_exists;
const err_not_supported: u16 = c.k_ra8_err_not_supported;
const err_busy: u16 = c.k_ra8_err_busy;

const entry_bytes: u32 = @intCast(c.k_ra8_fs_dir_entry_bytes);
const entry_off: usize = entry_bytes;
const off_name: usize = @intCast(c.k_dir_off_name);
const off_attr: usize = @intCast(c.k_dir_off_attr);
const off_ntres: usize = @intCast(c.k_dir_off_ntres);
const off_file_size: usize = @intCast(c.k_dir_off_file_size);
const marker_free_perm: u8 = @intCast(c.k_dir_marker_free_perm);
const marker_free_used: u8 = @intCast(c.k_dir_marker_free_used);
const marker_dot: u8 = @intCast(c.k_dir_marker_dot);
const attr_lfn: u8 = @intCast(c.k_ra8_fs_attr_lfn);
const attr_directory: u8 = @intCast(c.k_ra8_fs_attr_directory);
const attr_read_only: u8 = @intCast(c.k_ra8_fs_attr_read_only);
const first_data: u32 = @intCast(c.k_cluster_first_data);
const fs_type_exfat: u8 = @intCast(c.k_ra8_fs_type_exfat);
const short_name_len: usize = @intCast(c.k_ra8_fs_short_name_len);
const lfn_utf8_cap: usize = @intCast(c.k_lfn_utf8_cap);
const dir_state_bytes: usize = @intCast(c.k_ra8_fs_dir_state_bytes);

extern fn priv_resolve_parent(m: *const Mount, path: [*:0]const u8, out_parent: *c.dir_loc_t, out_leaf: *[*:0]const u8) u16;
extern fn priv_resolve_dir(m: *const Mount, path: [*:0]const u8, out: *c.dir_loc_t) u16;
extern fn priv_dir_walk_init_loc(m: [*c]const Mount, loc: [*c]const c.dir_loc_t, w: [*c]c.dir_walk_t) void;
extern fn priv_dir_walk_next_sector(m: [*c]const Mount, w: [*c]c.dir_walk_t, out_eod: [*c]u8) u16;
extern fn priv_lfn_reset(s: *c.lfn_state_t) void;
extern fn priv_lfn_add(s: *c.lfn_state_t, ent: [*]const u8) void;
extern fn priv_lfn_units_for(s: *const c.lfn_state_t, name83: [*]const u8, out_units: *u32) ?[*]const u16;
extern fn priv_83_to_str(in11: [*c]const u8, ntres: u8, out13: [*c]u8) void;
extern fn priv_utf16_to_utf8(in: ?[*]const u16, units: u32, out: ?[*]u8, cap: u32) u16;
extern fn priv_dir_eps(m: [*c]const Mount) u32;
extern fn priv_bps(m: [*c]const Mount) u32;
extern fn priv_read_sector(m: [*c]const Mount, lba: u64, buf: [*c]u8) u16;
extern fn priv_rd32(p: [*c]const u8) u32;
extern fn priv_dir_lookup_any(m: [*c]const Mount, loc: [*c]const c.dir_loc_t, leaf: [*c]const u8, out_lba: [*c]u64, out_entry_off: [*c]u32, out_entry: [*c]u8) u16;
extern fn priv_dir_erase_chain(m: [*c]const Mount, loc: [*c]const c.dir_loc_t, lba: u64, off: u32, name83: [*c]const u8) u16;
extern fn priv_dir_reserve(m: [*c]const Mount, loc: [*c]const c.dir_loc_t, leaf: [*c]const u8, out: [*c]c.dir_insert_t) u16;
extern fn priv_dir_commit(m: [*c]const Mount, plan: [*c]const c.dir_insert_t, tmpl: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32) u16;
extern fn priv_free_chain(m: *const Mount, start: u32) u16;
extern fn priv_entry_first_cluster(entry: [*]const u8) u32;
extern fn priv_fat_entry_stamp_access(entry: [*c]u8) void;
extern fn priv_sec_walk() [*c]u8;
extern fn priv_lock_acquire() void;
extern fn priv_lock_release() void;
extern fn priv_exfat_resolve_dir(m: [*c]const Mount, path: [*c]const u8, out: [*c]c.exfat_dir_t) u16;
extern fn priv_exfat_cursor_init(dir: *const c.exfat_dir_t, out: *c.exfat_cursor_t) void;
extern fn priv_exfat_listdir(m: [*c]const Mount, dir: [*c]const c.exfat_dir_t, cb: c.ra8_fs_listdir_cb_t, ctx: ?*anyopaque) u16;
extern fn priv_exfat_dir_next(m: [*c]const Mount, cur: [*c]c.exfat_cursor_t, out: [*c]c.ra8_fs_dirent_t, out_entry: [*c]bool) u16;
extern fn priv_exfat_unlink(m: [*c]const Mount, path: [*c]const u8) u16;
extern fn priv_exfat_rename(m: [*c]const Mount, old_path: [*c]const u8, new_path: [*c]const u8) u16;

/// Private state stored inside one public opaque directory cursor. The
/// public `ra8_fs_dir_t` hands the caller 640 bytes of it; these members are
/// the same four the C kept, so the size assertion below carries over.
const DirPrivate = extern struct {
    mount: ?*const Mount,
    fat_walk: c.dir_walk_t,
    fat_lfn: c.lfn_state_t,
    exfat_cursor: c.exfat_cursor_t,
    finished: bool,
};

comptime {
    std.debug.assert(@sizeOf(DirPrivate) <= dir_state_bytes);
    std.debug.assert(@alignOf(DirPrivate) <= 8);
}

/// Recover the private cursor state from one public directory handle.
fn state_of(directory: [*c]c.ra8_fs_dir_t) *DirPrivate {
    const bytes: [*]u8 = &directory.*.state[0];
    return @ptrCast(@alignCast(bytes));
}

/// Visit every visible entry in one already-loaded directory sector: skip
/// deleted (0xE5) and LFN (attr 0x0F) entries and the synthetic "." / ".."
/// directory entries, stop on the end-of-directory marker (0x00), and hand
/// the caller one callback per visible entry. Returns 1 when the
/// end-of-directory marker was hit so the caller can stop.
fn visit_sector(m: [*c]const Mount, buf: [*c]u8, lfn: *c.lfn_state_t, cb: c.ra8_fs_listdir_cb_t, ctx: ?*anyopaque) u8 {
    const eps = priv_dir_eps(m);
    var e: u32 = 0;
    while (e < eps) : (e += 1) {
        const ent: [*]const u8 = buf + @as(usize, e) * entry_off;
        if (ent[off_name] == marker_free_perm) {
            return 1; // end-of-directory: the caller stops.
        }
        if (ent[off_name] == marker_free_used) {
            priv_lfn_reset(lfn); // a deleted slot breaks any in-progress chain
            continue;
        }
        if (ent[off_attr] == attr_lfn) {
            priv_lfn_add(lfn, ent);
            continue;
        }
        if (ent[off_name] == marker_dot) {
            priv_lfn_reset(lfn); // synthetic "." / ".." -- not reported to the caller
            continue;
        }
        // +1 for the NUL: a maximal 8.3 name "12345678.123" is 12 chars, and
        // priv_83_to_str writes the terminator at index 12 -- without the
        // slack that store would run into the next byte of the buffer.
        var short_name: [short_name_len + 1]u8 = @splat(0);
        // The NTRes case flags come along: an entry this driver wrote for
        // `data.log` stores `DATA    LOG` with both flags set, and reporting
        // it as `DATA.LOG` would lose the caller's own name for no reason.
        priv_83_to_str(&ent[off_name], ent[off_ntres], &short_name);
        // Report the VFAT long name when the preceding chain matches; else
        // the 8.3. The chain is UTF-16 on disk and UTF-8 at this boundary,
        // and a chain that does not convert -- an unpaired surrogate, which
        // nothing conforming writes -- falls back to the alias rather than
        // being reported under a name that would not re-open it.
        var lname: [lfn_utf8_cap]u8 = @splat(0);
        var lnunits: u32 = 0;
        var have_long = false;
        const lunits = priv_lfn_units_for(lfn, &ent[off_name], &lnunits);
        if (lunits != null) {
            have_long = priv_utf16_to_utf8(lunits, lnunits, &lname, @intCast(lfn_utf8_cap)) == ok;
        }
        const size: u64 = priv_rd32(&ent[off_file_size]);
        const long_ptr: [*c]const u8 = &lname;
        const short_ptr: [*c]const u8 = &short_name;
        cb.?(if (have_long) long_ptr else short_ptr, ent[off_attr], size, ctx);
        priv_lfn_reset(lfn);
    }
    return 0;
}

/// Enumerate a directory: resolve `path` to a directory -- `priv_resolve_dir`
/// on FAT, `priv_exfat_resolve_dir` on exFAT -- and walk it, invoking `cb`
/// once per visible entry. Both filesystems take "/" or a nested path. FAT's
/// synthetic "." and ".." entries are not reported; exFAT has none to report.
fn internal_listdir_locked(handle: [*c]const Mount, path: [*c]const u8, cb: c.ra8_fs_listdir_cb_t, ctx: ?*anyopaque) u16 {
    if (handle == null or cb == null or path == null) {
        return err_null_ptr;
    }
    if (handle.*.in_use == 0) {
        return err_invalid_state;
    }
    if (handle.*.type == fs_type_exfat) {
        // Any exFAT directory, not just the root. A path that names nothing
        // reports not_found and one that names a FILE reports invalid_arg,
        // which is what the FAT side has always answered -- the two
        // filesystems no longer disagree about what "list this path" means.
        var dir: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
        const xerr = priv_exfat_resolve_dir(handle, path, &dir);
        if (xerr != ok) {
            return xerr;
        }
        return priv_exfat_listdir(handle, &dir, cb, ctx);
    }
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    const rerr = priv_resolve_dir(handle, path, &loc);
    if (rerr != ok) {
        return rerr;
    }
    var w: c.dir_walk_t = std.mem.zeroes(c.dir_walk_t);
    priv_dir_walk_init_loc(handle, &loc, &w);
    var lfn: c.lfn_state_t = std.mem.zeroes(c.lfn_state_t);
    priv_lfn_reset(&lfn); // persists across sectors -- LFN chains can straddle them
    var eod: u8 = 0;
    const buf: [*c]u8 = priv_sec_walk();
    while (eod == 0) {
        const err = priv_read_sector(handle, w.cur_lba, buf);
        if (err != ok) {
            return err;
        }
        if (visit_sector(handle, buf, &lfn, cb, ctx) != 0) {
            return ok;
        }
        const step = priv_dir_walk_next_sector(handle, &w, &eod);
        if (step != ok) {
            return step;
        }
    }
    return ok;
}

/// Advance an independent FAT cursor to its next directory sector:
/// delegates the format-specific sector walk and records clean end in the
/// stable cursor before returning the walk result. An error is returned
/// unchanged even when the walker also reports end.
fn internal_fat_dir_advance_sector(state: *DirPrivate) u16 {
    var end: u8 = 0;
    const step = priv_dir_walk_next_sector(state.mount, &state.fat_walk, &end);
    if (end != 0) {
        state.finished = true;
    }
    return step;
}

/// Scan one already-read sector's remaining entries for one visible entry:
/// advance the entry cursor across free/used/LFN/dot entries, assembling
/// valid LFN fragments, until a visible entry is copied, the directory's end
/// marker is found, or the sector is exhausted. `state.fat_walk.entry_idx`
/// is a byte offset and ends past every entry inspected.
fn internal_fat_dir_scan_sector(state: *DirPrivate, sector: [*]const u8, out: [*c]c.ra8_fs_dirent_t, out_entry: *bool) void {
    const bps = priv_bps(state.mount);
    while (state.fat_walk.entry_idx < bps) {
        const entry: [*]const u8 = sector + @as(usize, state.fat_walk.entry_idx);
        state.fat_walk.entry_idx += entry_bytes;
        if (entry[off_name] == marker_free_perm) {
            state.finished = true;
            return;
        }
        if (entry[off_name] == marker_free_used) {
            priv_lfn_reset(&state.fat_lfn);
            continue;
        }
        if (entry[off_attr] == attr_lfn) {
            priv_lfn_add(&state.fat_lfn, entry);
            continue;
        }
        if (entry[off_name] == marker_dot) {
            priv_lfn_reset(&state.fat_lfn);
            continue;
        }
        var short_name: [short_name_len + 1]u8 = @splat(0);
        var long_name: [lfn_utf8_cap]u8 = @splat(0);
        priv_83_to_str(&entry[off_name], entry[off_ntres], &short_name);
        var units: u32 = 0;
        const name_units = priv_lfn_units_for(&state.fat_lfn, &entry[off_name], &units);
        const short_ptr: [*c]const u8 = &short_name;
        const long_ptr: [*c]const u8 = &long_name;
        var name: [*c]const u8 = short_ptr;
        if (name_units != null) {
            if (priv_utf16_to_utf8(name_units, units, &long_name, @intCast(lfn_utf8_cap)) == ok) {
                name = long_ptr;
            }
        }
        var n: usize = 0;
        while (name[n] != 0) : (n += 1) {}
        @memcpy(out.*.name[0 .. n + 1], name[0 .. n + 1]);
        out.*.attr = entry[off_attr];
        out.*.size_bytes = priv_rd32(&entry[off_file_size]);
        priv_lfn_reset(&state.fat_lfn);
        out_entry.* = true;
        return;
    }
}

/// Copy one visible FAT entry from an independent cursor: walk bounded
/// directory sectors, assemble valid LFN fragments, and copy one stable
/// name/attribute/size value per successful step. The cursor advances
/// monotonically and `out` never borrows sector scratch; a sector is
/// re-read on a later call, so total work stays linear in entries.
fn internal_fat_dir_next(state: *DirPrivate, out: [*c]c.ra8_fs_dirent_t, out_entry: *bool) u16 {
    out_entry.* = false;
    const bps = priv_bps(state.mount);
    while (!state.finished) {
        if (state.fat_walk.entry_idx >= bps) {
            const step = internal_fat_dir_advance_sector(state);
            if (step != ok) {
                return step;
            }
            if (state.finished) {
                return ok;
            }
        }
        const sector: [*c]u8 = priv_sec_walk();
        const err = priv_read_sector(state.mount, state.fat_walk.cur_lba, sector);
        if (err != ok) {
            return err;
        }
        internal_fat_dir_scan_sector(state, sector, out, out_entry);
        if (out_entry.* or state.finished) {
            return ok;
        }
    }
    return ok;
}

/// Resolve and initialize one private filesystem cursor: select FAT or exFAT
/// path resolution from the mounted format and initialize only the matching
/// caller-owned walk state. Success initializes exactly one format-specific
/// cursor; failure publishes no open public cursor handle.
fn internal_dir_open_locked(handle: [*c]Mount, path: [*c]const u8, state: *DirPrivate) u16 {
    if (handle.*.in_use == 0) {
        return err_invalid_state;
    }
    state.* = std.mem.zeroes(DirPrivate);
    state.mount = handle;
    var err: u16 = ok;
    if (handle.*.type == fs_type_exfat) {
        var dir: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
        err = priv_exfat_resolve_dir(handle, path, &dir);
        if (err == ok) {
            priv_exfat_cursor_init(&dir, &state.exfat_cursor);
        }
    } else {
        var location: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
        err = priv_resolve_dir(handle, path, &location);
        if (err == ok) {
            priv_dir_walk_init_loc(handle, &location, &state.fat_walk);
            priv_lfn_reset(&state.fat_lfn);
        }
    }
    return err;
}

/// Resolve a path to a deletable FILE's entry and first cluster: resolve the
/// parent, look the leaf up by either of its on-disk names, and require the
/// matched entry NOT to carry the directory attribute. The mirror image of
/// `priv_rmdir_locate`: each verb refuses the kind of entry the other owns,
/// and both refuse before anything is freed.
fn internal_unlink_locate(handle: [*c]const Mount, path: [*c]const u8, out: *c.dir_target_t, out_cluster: *u32) u16 {
    var leaf: [*:0]const u8 = undefined;
    const rerr = priv_resolve_parent(handle, path, &out.parent, &leaf);
    if (rerr != ok) {
        return rerr;
    }
    const err = priv_dir_lookup_any(handle, &out.parent, leaf, &out.lba, &out.off, &out.entry[0]);
    if (err != ok) {
        return err;
    }
    // The lookup matches on the name alone, so a directory matches like any
    // other entry. Freeing its chain orphans every file inside it -- their
    // clusters stay allocated in the FAT with nothing referencing them,
    // which fsck.fat reports as lost clusters.
    if ((out.entry[off_attr] & attr_directory) != 0) {
        return err_invalid_arg; // a directory: ra8_fs_rmdir() is the verb
    }
    // Honor the read-only attribute: a file a host marked read-only is not
    // deletable through the ordinary path, matching DOS/Windows `del`.
    // Refused before priv_free_chain() touches the FAT, so a denied unlink
    // changes nothing.
    if ((out.entry[off_attr] & attr_read_only) != 0) {
        return err_access_denied;
    }
    out_cluster.* = priv_entry_first_cluster(&out.entry[0]);
    return ok;
}

/// Delete a file: free the cluster chain (if any) and mark the dir entry
/// deleted by writing 0xE5 to the first byte of the name field. Refuses a
/// directory: use `ra8_fs_rmdir()` for those.
fn internal_unlink_locked(handle: [*c]const Mount, path: [*c]const u8) u16 {
    if (handle == null or path == null) {
        return err_null_ptr;
    }
    if (handle.*.in_use == 0) {
        return err_invalid_state;
    }
    if (handle.*.type == fs_type_exfat) {
        return priv_exfat_unlink(handle, path);
    }
    var t: c.dir_target_t = std.mem.zeroes(c.dir_target_t);
    var cluster: u32 = 0;
    const lerr = internal_unlink_locate(handle, path, &t, &cluster);
    if (lerr != ok) {
        return lerr;
    }
    if (cluster >= first_data) {
        const ferr = priv_free_chain(handle, cluster);
        if (ferr != ok) {
            return ferr;
        }
    }
    // The whole chain, not only the 8.3 entry: clearing one slot left the
    // file's long-name entries on disk with a checksum nothing matched,
    // which fsck.fat and chkdsk both report as orphans.
    return priv_dir_erase_chain(handle, &t.parent, t.lba, t.off, &t.entry[off_name]);
}

/// Resolve a rename's old/new paths to a shared parent and two leaves:
/// resolve both parents and require them to be the same directory, since an
/// in-place rename cannot move an entry between directories. The leaf
/// components are handed back unpacked, because whether either of them is an
/// 8.3 name is no longer the rename's business -- the reservation decides
/// that, and both sides may be long names.
fn internal_rename_prepare(handle: [*c]const Mount, old_path: [*c]const u8, new_path: [*c]const u8, out_parent: *c.dir_loc_t, out_old: *[*:0]const u8, out_new: *[*:0]const u8) u16 {
    var op: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var ol: [*:0]const u8 = undefined;
    const e1 = priv_resolve_parent(handle, old_path, &op, &ol);
    if (e1 != ok) {
        return e1;
    }
    var np: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var nl: [*:0]const u8 = undefined;
    const e2 = priv_resolve_parent(handle, new_path, &np, &nl);
    if (e2 != ok) {
        return e2;
    }
    if (op.is_root != np.is_root) {
        return err_not_supported;
    }
    if (op.is_root == 0 and op.cluster != np.cluster) {
        return err_not_supported;
    }
    out_parent.* = op;
    out_old.* = ol;
    out_new.* = nl;
    return ok;
}

/// Rename by writing the new entry first, then taking the old one away.
/// Rewriting the 11-byte name field in place, which is what this used to do,
/// cannot be right once long names exist: the entry may be the tail of a
/// VFAT chain that still spells the OLD name, and the new name may not fit
/// eleven bytes at all. So the entry is re-filed -- the old 32 bytes are
/// carried across verbatim as the template, so the cluster, the size and
/// the attributes survive, and only the name and its DIR_NTRes flags change.
/// The order is deliberate: committing the new entry BEFORE erasing the old
/// one means a failure between them leaves the file reachable under both
/// names -- untidy, but nothing is lost. The reverse order would put a
/// window in the middle of the operation where the file has no name at all.
fn internal_fat_rename(handle: [*c]const Mount, old_path: [*c]const u8, new_path: [*c]const u8) u16 {
    var t: c.dir_target_t = std.mem.zeroes(c.dir_target_t);
    var ol: [*:0]const u8 = undefined;
    var nl: [*:0]const u8 = undefined;
    const perr = internal_rename_prepare(handle, old_path, new_path, &t.parent, &ol, &nl);
    if (perr != ok) {
        return perr;
    }
    var dup_lba: u64 = 0;
    var dup_off: u32 = 0;
    var dup: [entry_off]u8 = @splat(0);
    if (priv_dir_lookup_any(handle, &t.parent, nl, &dup_lba, &dup_off, &dup) == ok) {
        return err_exists;
    }
    var err = priv_dir_lookup_any(handle, &t.parent, ol, &t.lba, &t.off, &t.entry[0]);
    if (err != ok) {
        return err;
    }
    // Honor the read-only attribute: a read-only file is refused before any
    // new entry is filed or the old chain is erased, so the directory is
    // untouched.
    if ((t.entry[off_attr] & attr_read_only) != 0) {
        return err_access_denied;
    }
    var plan: c.dir_insert_t = std.mem.zeroes(c.dir_insert_t);
    err = priv_dir_reserve(handle, &t.parent, nl, &plan);
    if (err != ok) {
        return err;
    }
    // Access date only, deliberately. A rename changes the name, not the
    // bytes, so advancing DIR_WrtDate would tell every rsync, backup and
    // "newest image" OTA heuristic that the contents changed -- the same
    // class of false signal the timestamp fix exists to remove, only
    // inverted. FAT has no metadata-change field, so the access date is the
    // honest record that the entry was touched. It goes on the template
    // BEFORE the commit, because since long-name write a rename writes a new
    // entry rather than editing the old one in place.
    priv_fat_entry_stamp_access(&t.entry[0]);
    var new_lba: u64 = 0;
    var new_off: u32 = 0;
    err = priv_dir_commit(handle, &plan, &t.entry[0], &new_lba, &new_off);
    if (err != ok) {
        return err;
    }
    return priv_dir_erase_chain(handle, &t.parent, t.lba, t.off, &t.entry[off_name]);
}

/// Rename: dispatch to the FAT re-filing path or the exFAT entry-set
/// rewrite. On success the new name resolves to the same data; on failure
/// the directory is unchanged.
fn internal_rename_locked(handle: [*c]const Mount, old_path: [*c]const u8, new_path: [*c]const u8) u16 {
    if (handle == null or old_path == null or new_path == null) {
        return err_null_ptr;
    }
    if (handle.*.in_use == 0) {
        return err_invalid_state;
    }
    if (handle.*.type == fs_type_exfat) {
        return priv_exfat_rename(handle, old_path, new_path);
    }
    return internal_fat_rename(handle, old_path, new_path);
}

// =============================================================================
// Public entry points -- the lock brackets
// =============================================================================

pub export fn ra8_fs_listdir(handle: [*c]const Mount, path: [*c]const u8, cb: c.ra8_fs_listdir_cb_t, ctx: ?*anyopaque) callconv(.c) u16 {
    priv_lock_acquire();
    defer priv_lock_release();
    return internal_listdir_locked(handle, path, cb, ctx);
}

pub export fn ra8_fs_dir_open(handle: [*c]Mount, path: [*c]const u8, directory: [*c]c.ra8_fs_dir_t) callconv(.c) u16 {
    if (handle == null or path == null or directory == null) {
        return err_null_ptr;
    }
    if (directory.*.is_open) {
        return err_busy;
    }
    const state = state_of(directory);
    priv_lock_acquire();
    const err = internal_dir_open_locked(handle, path, state);
    priv_lock_release();
    if (err == ok) {
        directory.*.is_open = true;
    } else {
        @memset(&directory.*.state, 0);
    }
    return err;
}

pub export fn ra8_fs_dir_next(directory: [*c]c.ra8_fs_dir_t, out: [*c]c.ra8_fs_dirent_t, out_entry: [*c]bool) callconv(.c) u16 {
    if (directory == null or out == null or out_entry == null) {
        return err_null_ptr;
    }
    if (!directory.*.is_open) {
        return err_invalid_state;
    }
    out.* = std.mem.zeroes(c.ra8_fs_dirent_t);
    out_entry.* = false;
    const state = state_of(directory);
    priv_lock_acquire();
    const mounted = state.mount != null and state.mount.?.in_use != 0;
    var err: u16 = if (mounted) ok else err_invalid_state;
    if (err == ok and !state.finished) {
        if (state.mount.?.type == fs_type_exfat) {
            err = priv_exfat_dir_next(state.mount, &state.exfat_cursor, out, out_entry);
        } else {
            err = internal_fat_dir_next(state, out, out_entry);
        }
    }
    if (err == ok and !out_entry.*) {
        state.finished = true;
    }
    priv_lock_release();
    return err;
}

pub export fn ra8_fs_dir_close(directory: [*c]c.ra8_fs_dir_t) callconv(.c) u16 {
    if (directory == null) {
        return err_null_ptr;
    }
    if (!directory.*.is_open) {
        return err_invalid_state;
    }
    directory.* = std.mem.zeroes(c.ra8_fs_dir_t);
    return ok;
}

pub export fn ra8_fs_unlink(handle: [*c]const Mount, path: [*c]const u8) callconv(.c) u16 {
    priv_lock_acquire();
    defer priv_lock_release();
    return internal_unlink_locked(handle, path);
}

pub export fn ra8_fs_rename(handle: [*c]const Mount, old_path: [*c]const u8, new_path: [*c]const u8) callconv(.c) u16 {
    priv_lock_acquire();
    defer priv_lock_release();
    return internal_rename_locked(handle, old_path, new_path);
}
