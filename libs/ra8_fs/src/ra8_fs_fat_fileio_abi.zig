//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! FAT read / write / seek / tell / size I/O.
//!
//! Port of `ra8_fs_fat_fileio.c`. The read side positions to the cluster
//! covering the file offset (a sequential-read waypoint cache keeps a long
//! read from re-walking the chain from the head), serves zeros past exFAT's
//! `ValidDataLength`, and streams sector slices out. The write side grows
//! the chain cluster by cluster with its own forward waypoint, patches the
//! on-disk dir entry with the new cluster and size, and refuses a write
//! that would push a FAT file past the 4 GiB `DIR_FileSize` ceiling.
//! `priv_alloc_eoc_cluster` and `priv_write_into_sector` live here too:
//! every other port that grows a chain or does a read-modify-write reaches
//! them through these exports.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;
pub const File = c.ra8_fs_file_t;
pub const Mode = c.ra8_fs_mode_t;

const ok: u16 = c.k_ra8_ok;
const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
const err_invalid_size: u16 = c.k_ra8_err_invalid_size;

const first_data: u32 = @intCast(c.k_cluster_first_data);
const max_file_bytes: u64 = @intCast(c.k_ra8_fs_fat_max_file_bytes);
const type_exfat = c.k_ra8_fs_type_exfat;
const mode_read = c.k_ra8_fs_mode_read;
const mode_write = c.k_ra8_fs_mode_write;
const attr_archive: u8 = @intCast(c.k_ra8_fs_attr_archive);

extern fn priv_fat_get(m: [*c]const Mount, cluster: u32, out_value: [*c]u32) callconv(.c) u16;
extern fn priv_fat_set(m: [*c]const Mount, cluster: u32, value: u32) callconv(.c) u16;
extern fn priv_is_eoc(m: [*c]const Mount, value: u32) callconv(.c) u8;
extern fn priv_eoc_write(m: [*c]const Mount) callconv(.c) u32;
extern fn priv_alloc_cluster(m: [*c]const Mount, out_cluster: [*c]u32) callconv(.c) u16;
extern fn priv_cluster_bytes(m: [*c]const Mount) callconv(.c) u32;
extern fn priv_bps(m: [*c]const Mount) callconv(.c) u32;
extern fn priv_cluster_to_lba(m: [*c]const Mount, cluster: u32) callconv(.c) u64;
extern fn priv_byte_copy(dst: [*c]u8, src: [*c]const u8, n: u32) callconv(.c) void;
extern fn priv_sec_io() callconv(.c) [*c]u8;
extern fn priv_sec_walk() callconv(.c) [*c]u8;
extern fn priv_read_sector(m: [*c]const Mount, lba: u64, buf: [*c]u8) callconv(.c) u16;
extern fn priv_write_sector(m: [*c]const Mount, lba: u64, buf: [*c]const u8) callconv(.c) u16;
extern fn priv_entry_set_cluster_size(entry: [*c]u8, cluster: u32, size: u32) callconv(.c) void;
extern fn priv_fat_entry_stamp_write(entry: [*c]u8) callconv(.c) void;
extern fn priv_fat_entry_apply_attr(entry: [*c]u8, set_mask: u8, clear_mask: u8) callconv(.c) void;
extern fn priv_exfat_write_stream(file: [*c]File, buf: [*c]const u8, len: u32) callconv(.c) u16;
extern fn priv_exfat_flush_set(file: [*c]File) callconv(.c) u16;
extern fn priv_open_locked(handle: ?*Mount, path: ?[*:0]const u8, mode: Mode, out_file: ?*?*File) callconv(.c) u16;
extern fn priv_close_locked(file: ?*File) callconv(.c) u16;
extern fn priv_lock_acquire() callconv(.c) void;
extern fn priv_lock_release() callconv(.c) void;

/// Walk `n` clusters forward from `start` along the FAT chain. The read
/// path uses this to position to the cluster covering `file->offset`; it
/// stops early on EOC and reports where it got to.
pub export fn internal_skip_clusters(m: [*c]const Mount, start: u32, n: u32, out: [*c]u32) callconv(.c) u16 {
    var cur = start;
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        var next: u32 = 0;
        const e = priv_fat_get(m, cur, &next);
        if (e != ok) {
            return e;
        }
        if (priv_is_eoc(m, next) != 0) {
            out.* = cur;
            return err_invalid_state;
        }
        cur = next;
    }
    out.* = cur;
    return ok;
}

/// Read up to one sector's worth of bytes at the file's current offset:
/// resolve the cluster covering `file->offset`, read the containing sector,
/// copy the relevant slice into `buf`. The offset is NOT advanced here;
/// the caller does that.
///
/// The read accelerator: a sequential read advances the offset one cluster
/// at a time, so a FAT file resumes the walk from the cached forward
/// waypoint (`walk_cache_cluster` at `walk_cache_idx`) instead of
/// re-walking from the chain head on every sector -- O(n) over a file,
/// not the O(n^2) a per-sector walk-from-head would cost. The cache is
/// used only when it names a real data cluster AND the target is at or
/// ahead of it; a backward seek re-walks from the head. exFAT contiguous
/// files (`no_fat_chain`) have no valid FAT chain at all: the clusters are
/// sequential from the first one.
pub export fn internal_read_one_chunk(file: [*c]File, buf: [*c]u8, remaining: u32, out_take: [*c]u32) callconv(.c) u16 {
    const m = file[0].mount;
    const cluster_bytes = priv_cluster_bytes(m);
    const cluster_idx_now: u32 = @intCast(file[0].offset / cluster_bytes);
    var target: u32 = 0;
    if (file[0].no_fat_chain != 0) {
        target = file[0].first_cluster + cluster_idx_now;
    } else {
        var walk_from = file[0].first_cluster;
        var walk_n = cluster_idx_now;
        if (file[0].walk_cache_cluster >= first_data and cluster_idx_now >= file[0].walk_cache_idx) {
            walk_from = file[0].walk_cache_cluster;
            walk_n = cluster_idx_now - file[0].walk_cache_idx;
        }
        const werr = internal_skip_clusters(m, walk_from, walk_n, &target);
        if (werr != ok) {
            return werr;
        }
        file[0].walk_cache_idx = cluster_idx_now;
        file[0].walk_cache_cluster = target;
    }
    file[0].cur_cluster = target;
    const off_in_cluster: u32 = @intCast(file[0].offset % cluster_bytes);
    const bps = priv_bps(m);
    const sector_in_cluster = off_in_cluster / bps;
    const off_in_sector = off_in_cluster % bps;
    const lba = priv_cluster_to_lba(m, file[0].cur_cluster) + @as(u64, sector_in_cluster);
    const sec = priv_sec_io();
    const err = priv_read_sector(m, lba, sec);
    if (err != ok) {
        return err;
    }
    var take = bps - off_in_sector;
    if (take > remaining) {
        take = remaining;
    }
    priv_byte_copy(buf, sec + off_in_sector, take);
    out_take.* = take;
    return ok;
}

/// Read one span, serving zeros past exFAT's ValidDataLength (spec sec
/// 7.4.5): the prefix below `valid_bytes` was written, everything from
/// there to `DataLength` was never initialised and must read as zero -- the
/// clusters behind it still hold whatever the previous tenant left, so
/// serving them raw would hand a caller another file's data. A FAT handle
/// has no such split -- `DIR_FileSize` IS the written length -- so it takes
/// the same path with the two bounds equal and never reaches the hole arm.
pub export fn internal_read_span(file: [*c]File, buf: [*c]u8, remaining: u32, out_take: [*c]u32) callconv(.c) u16 {
    const m = file[0].mount;
    const valid: u64 = if (m[0].type == type_exfat) file[0].valid_bytes else file[0].size_bytes;
    if (file[0].offset >= valid) {
        var i: u32 = 0;
        while (i < remaining) : (i += 1) {
            buf[i] = 0;
        }
        out_take.* = remaining;
        return ok;
    }
    var want = remaining;
    const cap = valid - file[0].offset;
    if (want > cap) {
        want = @intCast(cap);
    }
    return internal_read_one_chunk(file, buf, want, out_take);
}

/// Read bytes -- the guarded body of `ra8_fs_read`. Loops on
/// `internal_read_span`, advancing the offset after each chunk; stops at
/// EOF or when `max_len` is met.
pub export fn internal_read_locked(file: [*c]File, buf: [*c]u8, max_len: u32, got_len: [*c]u32) callconv(.c) u16 {
    if (file == null or buf == null or got_len == null) {
        return err_null_ptr;
    }
    if (file[0].in_use == 0) {
        return err_invalid_state;
    }
    got_len.* = 0;
    if (file[0].offset >= file[0].size_bytes or max_len == 0) {
        return ok;
    }
    const left = file[0].size_bytes - file[0].offset;
    var remaining = max_len;
    if (max_len > left) {
        remaining = @intCast(left);
    }
    var produced: u32 = 0;
    while (remaining > 0) {
        var take: u32 = 0;
        const err = internal_read_span(file, buf + produced, remaining, &take);
        if (err != ok) {
            return err;
        }
        produced += take;
        file[0].offset += take;
        remaining -= take;
    }
    got_len.* = produced;
    return ok;
}

/// Allocate one cluster and point it at EOC in one step: the primitive
/// every chain-growth caller uses, so a new cluster can never be left
/// dangling with no end-of-chain marker.
pub export fn priv_alloc_eoc_cluster(m: [*c]const Mount, out_c: [*c]u32) callconv(.c) u16 {
    const err = priv_alloc_cluster(m, out_c);
    if (err != ok) {
        return err;
    }
    return priv_fat_set(m, out_c.*, priv_eoc_write(m));
}

/// Walk to cluster index `idx` from `start`, growing the chain as needed:
/// like `internal_skip_clusters`, but extends the chain with fresh EOC
/// clusters when EOC is met before `idx` is reached. On failure the FAT may
/// already carry part of the extension.
pub export fn internal_walk_grow(m: [*c]const Mount, start: u32, idx: u32, out_cluster: [*c]u32) callconv(.c) u16 {
    var cur = start;
    var i: u32 = 0;
    while (i < idx) : (i += 1) {
        var next: u32 = 0;
        var err = priv_fat_get(m, cur, &next);
        if (err != ok) {
            return err;
        }
        if (priv_is_eoc(m, next) != 0) {
            var newc: u32 = 0;
            err = priv_alloc_eoc_cluster(m, &newc);
            if (err != ok) {
                return err;
            }
            err = priv_fat_set(m, cur, newc);
            if (err != ok) {
                return err;
            }
            next = newc;
        }
        cur = next;
    }
    out_cluster.* = cur;
    return ok;
}

/// One read-modify-write of a sector slice: read the sector, copy `put`
/// bytes in at `off_in_sector`, write it back. The neighbours on the sector
/// survive untouched.
pub export fn priv_write_into_sector(m: [*c]const Mount, lba: u64, off_in_sector: u32, src: [*c]const u8, put: u32) callconv(.c) u16 {
    const sec = priv_sec_io();
    const err = priv_read_sector(m, lba, sec);
    if (err != ok) {
        return err;
    }
    priv_byte_copy(sec + off_in_sector, src, put);
    return priv_write_sector(m, lba, sec);
}

/// Forward waypoint carried across one write's grow-walks: without it every
/// sector of a long write re-walked the chain from the head, which is
/// O(K^2) FAT lookups over a K-cluster file -- the same quadratic as the
/// allocation scan and worth the same fix. One write's offset only ever
/// moves forward, so the waypoint is always at or behind the cluster being
/// written.
const WriteWay = struct {
    cluster: u32, // cluster the last walk reached
    index: u32, // chain index of `cluster`
};

/// Position the write cursor at chain index `idx`, growing as needed:
/// allocates the file's first cluster when it has none, then walks forward
/// from the waypoint -- not from the chain head -- extending the chain if
/// the walk runs off the end. The waypoint is advanced to wherever the walk
/// landed.
pub export fn internal_write_position(file: [*c]File, way: *WriteWay, idx: u32, out_cluster: [*c]u32) callconv(.c) u16 {
    const m = file[0].mount;
    if (file[0].first_cluster < first_data) {
        var first: u32 = 0;
        const aerr = priv_alloc_eoc_cluster(m, &first);
        if (aerr != ok) {
            return aerr;
        }
        file[0].first_cluster = first;
        file[0].cur_cluster = first;
        way.cluster = first;
        way.index = idx;
    }
    // A write's offset only moves forward within one call, so `way.index`
    // is at or behind `idx` by construction; an underflow here would be a
    // caller bug, not a short file.
    if (idx < way.index) {
        return err_invalid_state;
    }
    var cur: u32 = 0;
    const werr = internal_walk_grow(m, way.cluster, idx - way.index, &cur);
    if (werr != ok) {
        return werr;
    }
    way.cluster = cur;
    way.index = idx;
    out_cluster.* = cur;
    return ok;
}

/// Inner write loop: stream `len` bytes from `buf` into the file's chain.
/// Allocates the first cluster on demand, then walks/grows the chain as
/// needed and forwards each sector slice through `priv_write_into_sector`.
/// A write may allocate or grow the chain, so the read waypoint cache is
/// dropped up front.
pub export fn internal_write_stream(file: [*c]File, buf: [*c]const u8, len: u32) callconv(.c) u16 {
    const m = file[0].mount;
    const cluster_bytes = priv_cluster_bytes(m);
    const bps = priv_bps(m);
    var consumed: u32 = 0;
    file[0].walk_cache_cluster = 0;
    var way = WriteWay{ .cluster = file[0].first_cluster, .index = 0 };
    while (consumed < len) {
        const cluster_idx_now: u32 = @intCast(file[0].offset / cluster_bytes);
        var cur: u32 = 0;
        var err = internal_write_position(file, &way, cluster_idx_now, &cur);
        if (err != ok) {
            return err;
        }
        file[0].cur_cluster = cur;
        const off_in_cluster: u32 = @intCast(file[0].offset % cluster_bytes);
        const sector_in_cluster = off_in_cluster / bps;
        const off_in_sector = off_in_cluster % bps;
        const lba = priv_cluster_to_lba(m, file[0].cur_cluster) + @as(u64, sector_in_cluster);
        var put = bps - off_in_sector;
        if (put > len - consumed) {
            put = len - consumed;
        }
        err = priv_write_into_sector(m, lba, off_in_sector, buf + consumed, put);
        if (err != ok) {
            return err;
        }
        consumed += put;
        file[0].offset += put;
        if (file[0].offset > file[0].size_bytes) {
            file[0].size_bytes = file[0].offset;
        }
    }
    return ok;
}

/// Write bytes -- the guarded body of `ra8_fs_write`. Forwards to
/// `internal_write_stream`, then patches the on-disk dir entry with the
/// updated first-cluster and file size. exFAT handles stream through the
/// exFAT writer and flush their entry set instead; exFAT never reaches the
/// 4 GiB test below, because its branch has already returned.
pub export fn internal_write_locked(file: [*c]File, buf: [*c]const u8, len: u32) callconv(.c) u16 {
    if (file == null or buf == null) {
        return err_null_ptr;
    }
    if (file[0].in_use == 0 or file[0].mode == mode_read) {
        return err_invalid_state;
    }
    if (len == 0) {
        return ok;
    }
    const m = file[0].mount;
    if (m[0].type == type_exfat) {
        const xe = priv_exfat_write_stream(file, buf, len);
        if (xe != ok) {
            return xe;
        }
        file[0].dirty = 1;
        return priv_exfat_flush_set(file);
    }
    // The FAT boundary of the 4 GiB cap: `DIR_FileSize` is 32-bit, so a
    // write that would push the file past 4 GiB - 1 cannot be recorded and
    // is refused whole -- truncating the count silently would corrupt the
    // entry on the next flush.
    if (@as(u64, len) > max_file_bytes - file[0].offset) {
        return err_invalid_size;
    }
    var err = internal_write_stream(file, buf, len);
    if (err != ok) {
        return err;
    }
    const dirsec = priv_sec_walk();
    err = priv_read_sector(m, file[0].dir_entry_lba, dirsec);
    if (err != ok) {
        return err;
    }
    const at: [*c]u8 = dirsec + file[0].dir_entry_idx;
    priv_entry_set_cluster_size(at, file[0].first_cluster, @intCast(file[0].size_bytes));
    // The contents just changed, so the modification time has to move with
    // them. Doing it here as well as at close means a writer that is cut
    // off mid-stream still leaves an mtime describing what is actually on
    // the card, rather than whatever a PC wrote when it created the file.
    priv_fat_entry_stamp_write(at);
    // Same reason the archive attribute rides along: the file was modified,
    // which is exactly what the bit records.
    priv_fat_entry_apply_attr(at, attr_archive, 0);
    file[0].dirty = 1;
    return priv_write_sector(m, file[0].dir_entry_lba, dirsec);
}

/// Create a whole file -- the guarded body of `ra8_fs_write_file`. Drives
/// the *unlocked* open / write / close bodies: taking the library lock
/// again here would deadlock a non-recursive mutex. The public
/// `ra8_fs_write_file` is the wrapper that holds the lock across all three,
/// so the whole creation is one atomic operation rather than three. There
/// is no second, exFAT-only path: exFAT streams through the same three
/// calls now that `ra8_fs_open` accepts a writing mode on it -- two
/// implementations of one verb are two places for it to mean different
/// things.
pub export fn internal_write_file_locked(handle: ?*Mount, path: ?[*:0]const u8, data: [*c]const u8, len: u32) callconv(.c) u16 {
    if (handle == null or path == null or data == null) {
        return err_null_ptr;
    }
    if (handle[0].in_use == 0) {
        return err_invalid_state;
    }
    var f: ?*File = null;
    const e = priv_open_locked(handle, path, mode_write, &f);
    if (e != ok) {
        return e;
    }
    const werr = internal_write_locked(f, data, len);
    const cerr = priv_close_locked(f);
    if (werr != ok) {
        return werr;
    }
    return cerr;
}

/// Move the cursor -- the guarded body of `ra8_fs_seek`. Clamps the
/// requested offset to the current file size; the driver does not implement
/// sparse files. No on-disk state is modified.
pub export fn internal_seek_locked(file: [*c]File, offset_bytes: u64) callconv(.c) u16 {
    if (file == null) {
        return err_null_ptr;
    }
    if (file[0].in_use == 0) {
        return err_invalid_state;
    }
    var target = offset_bytes;
    if (target > file[0].size_bytes) {
        target = file[0].size_bytes;
    }
    file[0].offset = target;
    return ok;
}

/// Report the cursor -- the guarded body of `ra8_fs_tell`.
pub export fn internal_tell_locked(file: [*c]const File, out_offset: [*c]u64) callconv(.c) u16 {
    if (file == null or out_offset == null) {
        return err_null_ptr;
    }
    if (file[0].in_use == 0) {
        return err_invalid_state;
    }
    out_offset.* = file[0].offset;
    return ok;
}

/// Report the size -- the guarded body of `ra8_fs_size`.
pub export fn internal_size_locked(file: [*c]const File, out_bytes: [*c]u64) callconv(.c) u16 {
    if (file == null or out_bytes == null) {
        return err_null_ptr;
    }
    if (file[0].in_use == 0) {
        return err_invalid_state;
    }
    out_bytes.* = file[0].size_bytes;
    return ok;
}

// ============================================================================
// Public entry points -- the lock brackets
// ============================================================================

pub export fn ra8_fs_read(file: [*c]File, buf: [*c]u8, max_len: u32, got_len: [*c]u32) callconv(.c) u16 {
    priv_lock_acquire();
    const err = internal_read_locked(file, buf, max_len, got_len);
    priv_lock_release();
    return err;
}

pub export fn ra8_fs_write(file: [*c]File, buf: [*c]const u8, len: u32) callconv(.c) u16 {
    priv_lock_acquire();
    const err = internal_write_locked(file, buf, len);
    priv_lock_release();
    return err;
}

pub export fn ra8_fs_write_file(handle: ?*Mount, path: ?[*:0]const u8, data: [*c]const u8, len: u32) callconv(.c) u16 {
    priv_lock_acquire();
    const err = internal_write_file_locked(handle, path, data, len);
    priv_lock_release();
    return err;
}

pub export fn ra8_fs_seek(file: [*c]File, offset_bytes: u64) callconv(.c) u16 {
    priv_lock_acquire();
    const err = internal_seek_locked(file, offset_bytes);
    priv_lock_release();
    return err;
}

pub export fn ra8_fs_tell(file: [*c]const File, out_offset: [*c]u64) callconv(.c) u16 {
    priv_lock_acquire();
    const err = internal_tell_locked(file, out_offset);
    priv_lock_release();
    return err;
}

pub export fn ra8_fs_size(file: [*c]const File, out_bytes: [*c]u64) callconv(.c) u16 {
    priv_lock_acquire();
    const err = internal_size_locked(file, out_bytes);
    priv_lock_release();
    return err;
}
