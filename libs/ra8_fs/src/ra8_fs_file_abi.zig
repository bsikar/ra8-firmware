//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! FAT open / close path.
//!
//! Port of `ra8_fs_fat_file.c`. This is the unit callers reach first: `open`
//! resolves the path to a parent directory and a leaf, then either populates a
//! fresh handle from an on-disk entry or carves a new one, and `close` stamps
//! the final write time and flushes the FSInfo free count. The entry helpers
//! other ports lean on live here too: the first-cluster reader, the
//! cluster/size writer, and the attribute-bit setter.
//!
//! Path resolution stays in C (`ra8_fs_fat_resolve.c`) for now; the directory
//! finders, the insert planner, the stamp helpers and the exFAT open/flush
//! paths stay in C or in their own ports, reached through `fs_c.zig`. This
//! unit owns the handle lifecycle and the lock brackets.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;
pub const File = c.ra8_fs_file_t;
pub const DirLoc = c.dir_loc_t;
pub const DirInsert = c.dir_insert_t;
pub const Mode = c.ra8_fs_mode_t;

const ok: u16 = c.k_ra8_ok;
const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
const err_not_found: u16 = c.k_ra8_err_not_found;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const err_access_denied: u16 = c.k_ra8_err_access_denied;

const entry_bytes: usize = @intCast(c.k_ra8_fs_dir_entry_bytes);
const off_attr: usize = @intCast(c.k_dir_off_attr);
const off_fst_clus_hi: usize = @intCast(c.k_dir_off_fst_clus_hi);
const off_fst_clus_lo: usize = @intCast(c.k_dir_off_fst_clus_lo);
const off_file_size: usize = @intCast(c.k_dir_off_file_size);
const first_data: u32 = @intCast(c.k_cluster_first_data);
const attr_directory: u8 = @intCast(c.k_ra8_fs_attr_directory);
const attr_read_only: u8 = @intCast(c.k_ra8_fs_attr_read_only);
const attr_archive: u8 = @intCast(c.k_ra8_fs_attr_archive);
const mode_read = c.k_ra8_fs_mode_read;
const mode_write = c.k_ra8_fs_mode_write;
const mode_append = c.k_ra8_fs_mode_append;

extern fn priv_resolve_parent(m: *const Mount, path: [*:0]const u8, out_parent: *DirLoc, out_leaf: *[*:0]const u8) u16;
extern fn priv_dir_lookup_any(m: *const Mount, loc: *const DirLoc, leaf: [*:0]const u8, out_lba: *u64, out_off: *u32, out_entry: [*]u8) u16;
extern fn priv_dir_reserve(m: *const Mount, loc: *const DirLoc, leaf: [*:0]const u8, out: *DirInsert) u16;
extern fn priv_dir_commit(m: *const Mount, plan: *const DirInsert, tmpl: [*]const u8, out_lba: *u64, out_off: *u32) u16;
extern fn priv_exfat_open(handle: *Mount, path: [*:0]const u8, mode: Mode, out: *?*File) u16;
extern fn priv_exfat_flush_set(file: *File) u16;
extern fn priv_sec_walk() [*]u8;
extern fn priv_read_sector(m: *const Mount, lba: u64, buf: [*]u8) u16;
extern fn priv_write_sector(m: *const Mount, lba: u64, buf: [*]const u8) u16;
extern fn priv_alloc_file_slot() ?*File;
extern fn priv_fat_entry_stamp_write(entry: [*]u8) void;
extern fn priv_fat_entry_stamp_create(entry: [*]u8) void;
extern fn priv_free_chain(m: *const Mount, start: u32) u16;
extern fn priv_fsinfo_flush(m: ?*const Mount) u16;
extern fn priv_lock_acquire() void;
extern fn priv_lock_release() void;

/// First data cluster of the entry: `DIR_FstClusHI:DIR_FstClusLO`, little
/// endian each half, high half shifted over the low.
pub export fn priv_entry_first_cluster(entry: [*]const u8) callconv(.c) u32 {
    const hi: u32 = std.mem.readInt(u16, entry[off_fst_clus_hi..][0..2], .little);
    const lo: u32 = std.mem.readInt(u16, entry[off_fst_clus_lo..][0..2], .little);
    return (hi << 16) | lo;
}

/// Point the entry at `cluster` and give it `size` bytes, in one write.
pub export fn priv_entry_set_cluster_size(entry: [*]u8, cluster: u32, size: u32) callconv(.c) void {
    std.mem.writeInt(u16, entry[off_fst_clus_hi..][0..2], @intCast(cluster >> 16), .little);
    std.mem.writeInt(u16, entry[off_fst_clus_lo..][0..2], @intCast(cluster & 0xFFFF), .little);
    std.mem.writeInt(u32, entry[off_file_size..][0..4], size, .little);
}

/// Apply an attribute-bit mask to the entry: set what `set_mask` names, clear
/// what `clear_mask` names, leave the rest alone.
pub export fn priv_fat_entry_apply_attr(entry: [*]u8, set_mask: u8, clear_mask: u8) callconv(.c) void {
    entry[off_attr] = (entry[off_attr] & ~clear_mask) | set_mask;
}

/// Truncate an existing file's chain and zero its dir-entry size: frees the
/// cluster chain, resets the in-memory file state, then writes a fresh dir
/// entry with cluster=0 and size=0.
fn truncate_existing(handle: *const Mount, f: *File, lba: u64, off: u32) u16 {
    if (f.first_cluster >= first_data) {
        const ferr = priv_free_chain(handle, f.first_cluster);
        if (ferr != ok) {
            return ferr;
        }
    }
    f.first_cluster = 0;
    f.cur_cluster = 0;
    f.walk_cache_idx = 0;
    f.walk_cache_cluster = 0; // < 2: no read cache for a fresh file
    f.size_bytes = 0;
    f.offset = 0;
    const buf = priv_sec_walk();
    const err = priv_read_sector(handle, lba, buf);
    if (err != ok) {
        return err;
    }
    priv_entry_set_cluster_size(buf + off, 0, 0);
    // Truncation IS a content change -- the file went from N bytes to zero -- so
    // the modification time has to move even if the caller never writes a byte
    // afterwards. Without this, `open(write)` + `close()` left a PC-authored
    // mtime describing contents that no longer exist.
    priv_fat_entry_stamp_write(buf + off);
    // A content change also sets the archive attribute, the FAT/exFAT
    // convention a backup tool clears and the OS re-sets on every modification.
    priv_fat_entry_apply_attr(buf + off, attr_archive, 0);
    return priv_write_sector(handle, lba, buf);
}

/// Populate a fresh file handle from an existing on-disk dir entry: allocates
/// a file slot, copies the entry's first cluster / size into it, sets the
/// requested mode, and applies truncate/append behaviour for write/append
/// modes.
fn open_existing(handle: *Mount, entry: [*]const u8, lba: u64, off: u32, mode: Mode, out_file: *?*File) u16 {
    // A directory is not a file. Without this the write path ran
    // truncate_existing() on it -- freeing the chain that held every child and
    // stamping cluster 0 / size 0 into an entry still flagged ATTR_DIRECTORY --
    // and the read path handed back a zero-byte handle, because a directory's
    // DIR_FileSize is 0 by specification. Rejected for every mode, before the
    // file slot is allocated, so a refused open consumes nothing.
    if ((entry[off_attr] & attr_directory) != 0) {
        return err_invalid_arg;
    }
    // Honor the read-only attribute. A writing open truncates or appends, so a
    // file a host marked read-only must be refused BEFORE the slot is taken
    // and BEFORE truncate_existing() frees its chain -- the DOS/Windows
    // semantics say `open(read-only, write)` is denied, not silently obeyed. A
    // read open is untouched: reading a read-only file is exactly what the bit
    // permits.
    if (mode != mode_read and (entry[off_attr] & attr_read_only) != 0) {
        return err_access_denied;
    }
    const f = priv_alloc_file_slot() orelse return err_no_mem;
    f.mount = handle;
    f.first_cluster = priv_entry_first_cluster(entry);
    f.cur_cluster = f.first_cluster;
    f.walk_cache_idx = 0; // read accelerator seeded at the chain head
    f.walk_cache_cluster = f.first_cluster;
    f.size_bytes = std.mem.readInt(u32, entry[off_file_size..][0..4], .little);
    f.dir_entry_lba = lba;
    f.dir_entry_idx = off;
    f.mode = mode;
    f.no_fat_chain = 0;
    f.dirty = 0;
    f.in_use = 1;
    if (mode == mode_write) {
        const err = truncate_existing(handle, f, lba, off);
        if (err != ok) {
            f.in_use = 0;
            return err;
        }
    } else if (mode == mode_append) {
        f.offset = f.size_bytes;
    } else {
        f.offset = 0;
    }
    out_file.* = f;
    return ok;
}

/// Populate a freshly allocated file slot for an empty new file: every field
/// of `f` at the empty-file initial state -- no clusters, zero size/offset,
/// the dir-entry location, the open mode, and the in-use flag.
fn init_new_file(f: *File, handle: *Mount, mode: Mode, free_lba: u64, free_off: u32) void {
    f.mount = handle;
    f.first_cluster = 0;
    f.cur_cluster = 0;
    f.walk_cache_idx = 0;
    f.walk_cache_cluster = 0; // < 2: no read cache for a fresh file
    f.size_bytes = 0;
    f.offset = 0;
    f.dir_entry_lba = free_lba;
    f.dir_entry_idx = free_off;
    f.mode = mode;
    f.no_fat_chain = 0;
    f.dirty = 0;
    f.in_use = 1;
}

/// Carve a fresh directory entry for `leaf` and populate a file handle:
/// reserves the directory slots the name needs, takes a file slot from the
/// static pool, commits an empty archive entry, and initialises the handle to
/// the empty-file state. The reservation runs before anything is written, so
/// a directory with no room leaves the volume untouched.
fn create_new(handle: *const Mount, parent: *const DirLoc, leaf: [*:0]const u8, mode: Mode, out_file: *?*File) u16 {
    var plan = std.mem.zeroes(DirInsert);
    var err = priv_dir_reserve(handle, parent, leaf, &plan);
    if (err != ok) {
        return err;
    }
    const f = priv_alloc_file_slot() orelse return err_no_mem;
    var tmpl: [entry_bytes]u8 = @splat(0);
    tmpl[off_attr] = attr_archive;
    priv_entry_set_cluster_size(&tmpl, 0, 0);
    // The template is zero-filled above, and a zero FAT date is not a date:
    // month and day are both 1-based, so 0x0000 claims month 0 of day 0 and
    // every host decodes it differently. Stamped here rather than inside
    // `priv_dir_commit()` because that same commit re-files an EXISTING entry
    // for `rename`, which must keep the creation date it already has.
    priv_fat_entry_stamp_create(&tmpl);
    var lba: u64 = 0;
    var off: u32 = 0;
    err = priv_dir_commit(handle, &plan, &tmpl, &lba, &off);
    if (err != ok) {
        return err;
    }
    init_new_file(f, @constCast(handle), mode, lba, off);
    out_file.* = f;
    return ok;
}

/// Open path, under the caller's lock: dispatch to exFAT when the volume is
/// one, otherwise resolve the parent and either open the existing entry or
/// create a new file (write/append modes only).
pub export fn priv_open_locked(handle: ?*Mount, path: ?[*:0]const u8, mode: Mode, out_file: ?*?*File) callconv(.c) u16 {
    if (handle == null or path == null or out_file == null) {
        return err_null_ptr;
    }
    const m = handle.?;
    if (m.in_use == 0) {
        return err_invalid_state;
    }
    if (m.type == c.k_ra8_fs_type_exfat) {
        return priv_exfat_open(m, path.?, mode, out_file.?);
    }
    var parent = std.mem.zeroes(DirLoc);
    var leaf: [*:0]const u8 = undefined;
    const rerr = priv_resolve_parent(m, path.?, &parent, &leaf);
    if (rerr != ok) {
        return rerr;
    }
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [entry_bytes]u8 = @splat(0);
    const err = priv_dir_lookup_any(m, &parent, leaf, &lba, &off, &entry);
    if (err == ok) {
        return open_existing(m, &entry, lba, off, mode, out_file.?);
    }
    if (err != err_not_found) {
        return err;
    }
    if (mode == mode_read) {
        return err_not_found;
    }
    // Creation no longer needs an 8.3-representable name: `priv_dir_reserve()`
    // generates the alias and reserves the chain's slots.
    return create_new(m, &parent, leaf, mode, out_file.?);
}

/// Stamp the final modification time of a file that was written. `ra8_fs_write`
/// already advances the modification time on every call, which is what
/// protects a file whose writer never gets to close it; this is the stamp
/// that says when the file was FINISHED, which is the one a host displays and
/// a sync tool compares. Also flushes the volume's FSInfo free count, because
/// a file that has just stopped growing is exactly when the count is worth
/// committing.
fn close_stamp(file: *File) u16 {
    const m: *const Mount = file.mount;
    // exFAT keeps its metadata in a checksummed entry set, not one 32-byte
    // directory entry, so the close flush is the same commit every write
    // already performs -- stamp, patch, checksum, write.
    if (m.type == c.k_ra8_fs_type_exfat) {
        return priv_exfat_flush_set(file);
    }
    const sec = priv_sec_walk();
    var err = priv_read_sector(m, file.dir_entry_lba, sec);
    if (err != ok) {
        return err;
    }
    priv_fat_entry_stamp_write(sec + file.dir_entry_idx);
    err = priv_write_sector(m, file.dir_entry_lba, sec);
    if (err != ok) {
        return err;
    }
    return priv_fsinfo_flush(m);
}

/// Close path, under the caller's lock: stamps and flushes a dirty handle,
/// then frees the slot. Never touches the volume through a handle that is
/// already closed or whose mount has gone: the dir-entry LBA would be read
/// against a mount slot that now describes some other card.
pub export fn priv_close_locked(file: ?*File) callconv(.c) u16 {
    if (file == null) {
        return err_null_ptr;
    }
    const f = file.?;
    var err = ok;
    if (f.dirty != 0 and f.in_use != 0 and f.mount != null and f.mount.*.in_use != 0) {
        err = close_stamp(f);
    }
    f.dirty = 0;
    f.in_use = 0;
    f.mount = null;
    return err;
}

/// Public entry point: open with the lock brackets.
pub export fn ra8_fs_open(handle: ?*Mount, path: ?[*:0]const u8, mode: Mode, out_file: ?*?*File) callconv(.c) u16 {
    priv_lock_acquire();
    const err = priv_open_locked(handle, path, mode, out_file);
    priv_lock_release();
    return err;
}

/// Public entry point: close with the lock brackets.
pub export fn ra8_fs_close(file: ?*File) callconv(.c) u16 {
    priv_lock_acquire();
    const err = priv_close_locked(file);
    priv_lock_release();
    return err;
}
