//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT directories: path resolution, `mkdir`, `rmdir`, ported from
//! `ra8_fs_fat_exfat_dir.c` (RA8FW-1126).
//!
//! Three pieces, in the order a nested path uses them:
//!
//! 1. Descent: `enter` looks one component up in the directory it is standing
//!    in and requires the Directory attribute; `priv_exfat_resolve_parent`
//!    drives it component by component, in the shape of the FAT side's
//!    `priv_resolve_parent`, and hands back the parent plus a pointer to the
//!    leaf. Iterative, never recursive (NASA P10 Rule 1).
//! 2. Creation: the same File + Stream + Name entry set with bit 4 of
//!    `FileAttributes` set, the contents living in the run the Stream entry
//!    names, and no "." / ".." entries at all -- the parent is implicit, so
//!    there is no dot-entry packing and no back-link to maintain. An all-zero
//!    cluster is already a valid empty directory, because entry type 0x00 is
//!    end-of-directory.
//! 3. Removal: `priv_exfat_rmdir` proves the directory holds nothing before it
//!    frees anything, so a refusal costs the volume nothing.
//!
//! The run mkdir allocates is contiguous and carries NoFatChain, which is why
//! `exfat_cursor_t` has a run bound: the exFAT specification declares the FAT
//! entries of a NoFatChain run invalid, and the ones left behind here read as
//! 0 -- not end-of-chain -- so a FAT-following walk would step into cluster 0.
//!
//! Cross-module calls go through `fs_c.zig`: the set lookup, entry-set drop
//! and cluster freeing are the mutate ABI's exports, the cursor machinery the
//! read ABI's, the bitmap and slot work the write ABI's, and the sector
//! primitives the prim ABI's.

const std = @import("std");
const c = @import("fs_c.zig").c;
const exfat_write = @import("ra8_fs_exfat_write_abi.zig");

pub const Mount = c.ra8_fs_mount_t;

pub const ok: u16 = c.k_ra8_ok;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_not_found: u16 = c.k_ra8_err_not_found;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const err_protocol: u16 = c.k_ra8_err_protocol_error;
const err_exists: u16 = c.k_ra8_err_exists;
const err_not_empty: u16 = c.k_ra8_err_not_empty;

const entry_bytes: usize = @intCast(c.k_exfat_entry_bytes);
const name_cap: u32 = @intCast(c.k_exfat_name_cap);
const name_u8_cap: u32 = @intCast(c.k_exfat_name_u8_cap);
const name_per_entry: u32 = @intCast(c.k_exfat_name_per_entry);
const set_max: u32 = @intCast(c.k_exfat_set_max_entries);
const max_set_bytes: u32 = @intCast(c.k_exfat_max_set_bytes);
const scan_limit: u32 = @intCast(c.k_exfat_scan_limit);
const path_depth: u32 = @intCast(c.k_exfat_path_depth);
const sector_max: usize = @intCast(c.k_ra8_fs_sector_max);

const entry_file: u8 = @intCast(c.k_exfat_entry_file);
const entry_stream: u8 = @intCast(c.k_exfat_entry_stream);
const entry_name: u8 = @intCast(c.k_exfat_entry_name);
const entry_eod: u8 = @intCast(c.k_exfat_entry_eod);
const inuse_bit: u8 = @intCast(c.k_exfat_inuse_bit);
const attr_directory: u8 = @intCast(c.k_exfat_attr_directory);
const secflag_alloc: u8 = @intCast(c.k_exfat_secflag_alloc);

const off_file_secnt: usize = @intCast(c.k_exfat_off_file_secnt);
const off_file_attr: usize = @intCast(c.k_exfat_off_file_attr);
const off_file_csum: usize = @intCast(c.k_exfat_off_file_csum);
const strm_off_flags: usize = @intCast(c.k_exfat_strm_off_flags);
const strm_off_nlen: usize = @intCast(c.k_exfat_strm_off_nlen);
const off_strm_hash: usize = @intCast(c.k_exfat_off_strm_hash);
const off_strm_valid: usize = @intCast(c.k_exfat_off_strm_valid);
const strm_off_clus: usize = @intCast(c.k_exfat_strm_off_clus);
const strm_off_dlen: usize = @intCast(c.k_exfat_strm_off_dlen);
const name_off: usize = @intCast(c.k_exfat_name_off);
const first_data: u32 = @intCast(c.k_cluster_first_data);

const zero_sector: [sector_max]u8 = @splat(0);

fn rd32(p: []const u8, off: usize) u32 {
    return std.mem.readInt(u32, p[off..][0..4], .little);
}

fn wr16(p: []u8, off: usize, v: u16) void {
    std.mem.writeInt(u16, p[off..][0..2], v, .little);
}

fn wr32(p: []u8, off: usize, v: u32) void {
    std.mem.writeInt(u32, p[off..][0..4], v, .little);
}

/// Descend into the named directory component within `cur`: the exFAT
/// counterpart of `priv_enter_subdir`.
///
/// Two bounds, in two domains, and both are needed. The BYTE bound sizes the
/// buffer, because `comp` is UTF-8 here and a 64-unit name is up to three
/// times that many bytes. The UNIT bound is the one the FORMAT imposes, so it
/// is checked after conversion -- 65 ASCII characters and 65 accented ones
/// are equally one over, and only the unit count says so.
fn enter(m: *const Mount, cur: *const c.exfat_dir_t, comp: []const u8, out: *c.exfat_dir_t) u16 {
    if (comp.len >= name_u8_cap) {
        return err_invalid_arg;
    }
    var namebuf: [name_u8_cap]u8 = @splat(0);
    @memcpy(namebuf[0..comp.len], comp);
    // Validate the component converts within the exFAT unit cap and is
    // well-formed UTF-8 before searching -- the byte bound above cannot see a
    // name short in bytes yet over the 64-UNIT cap.
    var units: [name_cap]u16 = @splat(0);
    var nunits: u32 = 0;
    const ce = c.priv_exfat_name_to_units(m, &namebuf, &units, &nunits);
    if (ce == err_no_mem) {
        return err_invalid_arg; // over the name cap: an argument error
    }
    if (ce != ok) {
        return ce;
    }
    // `priv_exfat_find_set` rather than `priv_exfat_find`, because a directory
    // that fills up has to REWRITE its own File-entry set to grow, and only
    // find_set reports where that set lives. `pos[0]` is the File entry;
    // carrying it in the descendant is what lets the grow path patch the
    // right metadata without a second walk of the parent.
    var pos: [set_max]c.exfat_setpos_t = @splat(std.mem.zeroes(c.exfat_setpos_t));
    var count: u32 = 0;
    var file_e: [entry_bytes]u8 = @splat(0);
    var strm: [entry_bytes]u8 = @splat(0);
    const e = c.priv_exfat_find_set(m, cur, &namebuf, &pos, set_max, &count, &file_e, &strm);
    if (e != ok) {
        return e;
    }
    if (file_e[off_file_attr] & attr_directory == 0) {
        return err_invalid_arg; // a file cannot be an intermediate component
    }
    if (rd32(&strm, strm_off_clus) < first_data) {
        return err_protocol; // a directory always owns a cluster
    }
    c.priv_exfat_dir_from_set(m, &strm, out);
    out.*.self_cluster = pos[0].cluster;
    out.*.self_index = pos[0].index;
    return ok;
}

/// See `ra8_fs_fat_exfat_dir_internal.h` for the documented contract.
pub export fn priv_exfat_resolve_parent(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out_parent: [*c]c.exfat_dir_t, out_leaf: [*c][*c]const u8) callconv(.c) u16 {
    var p = path;
    while (p[0] == '/') p += 1;
    var cur: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    c.priv_exfat_dir_root(m, &cur);
    var depth: u32 = 0;
    while (depth < path_depth) : (depth += 1) {
        var end = p;
        var nlen: u32 = 0;
        while (end[0] != 0) {
            if (end[0] == '/') break;
            end += 1;
            nlen += 1;
        }
        if (end[0] == 0) {
            out_parent.* = cur;
            out_leaf.* = p;
            return ok;
        }
        var next: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
        const e = enter(m, &cur, p[0..nlen], &next);
        if (e != ok) {
            return e;
        }
        cur = next;
        p = end;
        while (p[0] == '/') p += 1;
    }
    return err_invalid_arg; // deeper than k_exfat_path_depth
}

/// See `ra8_fs_fat_exfat_dir_internal.h` for the documented contract.
pub export fn priv_exfat_resolve_dir(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out: [*c]c.exfat_dir_t) callconv(.c) u16 {
    var p = path;
    while (p[0] == '/') p += 1;
    if (p[0] == 0) {
        c.priv_exfat_dir_root(m, out);
        return ok;
    }
    var parent: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var leaf: [*c]const u8 = null;
    const e = priv_exfat_resolve_parent(m, path, &parent, &leaf);
    if (e != ok) {
        return e;
    }
    const len = c.priv_strlen(leaf);
    if (len == 0) {
        out.* = parent; // trailing slash: the parent IS the directory
        return ok;
    }
    return enter(m, &parent, leaf[0..len], out);
}

/// See `ra8_fs_fat_exfat_dir_internal.h` for the documented contract.
pub export fn priv_exfat_lookup(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out_strm: [*c]u8, out_attr: [*c]u8) callconv(.c) u16 {
    var parent: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var leaf: [*c]const u8 = null;
    const e = priv_exfat_resolve_parent(m, path, &parent, &leaf);
    if (e != ok) {
        return e;
    }
    if (c.priv_strlen(leaf) == 0) {
        return err_invalid_arg; // the volume root is not an entry set
    }
    return c.priv_exfat_find(m, &parent, leaf, out_strm, out_attr);
}

/// Zero every sector of one cluster.
pub export fn priv_exfat_zero_cluster(m: [*c]const c.ra8_fs_mount_t, clus: u32) callconv(.c) u16 {
    const base = c.priv_cluster_to_lba(m, clus);
    var s: u32 = 0;
    const spc = m.*.sectors_per_cluster;
    while (s < spc) : (s += 1) {
        const e = c.priv_write_sector(m, base + s, &zero_sector);
        if (e != ok) {
            return e;
        }
    }
    return ok;
}

/// Rewrite every end-of-directory marker of one cluster as a deleted File
/// entry, so a walk stops one entry earlier and the cluster reads as a
/// genuinely empty directory.
pub export fn priv_exfat_seal_cluster(m: [*c]const c.ra8_fs_mount_t, clus: u32) callconv(.c) u16 {
    const unused: u8 = entry_file & ~inuse_bit;
    const base = c.priv_cluster_to_lba(m, clus);
    const bps = c.priv_bps(m);
    var s: u32 = 0;
    const spc = m.*.sectors_per_cluster;
    while (s < spc) : (s += 1) {
        const sec = c.priv_sec_io();
        var e = c.priv_read_sector(m, base + s, sec);
        if (e != ok) {
            return e;
        }
        var dirty = false;
        var off: usize = 0;
        while (off < bps) : (off += entry_bytes) {
            if (sec[off] == entry_eod) {
                sec[off] = unused;
                dirty = true;
            }
        }
        if (dirty) {
            e = c.priv_write_sector(m, base + s, sec);
            if (e != ok) {
                return e;
            }
        }
    }
    return ok;
}

/// Build the File + Stream + Name entry set a DIRECTORY needs.
///
/// The file-side builder in the write ABI makes a zero-length, cluster-less
/// set, because a streaming write starts with nothing and grows. A directory
/// is the opposite: it is born owning exactly one cluster and never grows
/// here, so its set carries the Directory attribute, the cluster, NoFatChain,
/// and a DataLength equal to the ALLOCATION -- an exFAT directory's length is
/// its allocation, not a byte count, and the specification requires a
/// directory's ValidDataLength to equal DataLength. Both are written.
fn buildDirSet(m: *const Mount, set: []u8, name: []const u16, nlen: u32, first: u32) u32 {
    const name_entries = (nlen + name_per_entry - 1) / name_per_entry;
    const sec_count = 1 + name_entries;
    const total: u32 = @intCast((1 + sec_count) * entry_bytes);
    @memset(set[0..total], 0);
    set[0] = entry_file;
    set[off_file_secnt] = @intCast(sec_count);
    wr16(set, off_file_attr, @as(u16, attr_directory));
    // Before the SetChecksum below, which covers these bytes.
    c.priv_exfat_file_stamp_create(set.ptr);
    const cbytes = c.priv_cluster_bytes(m);
    const strm = set[entry_bytes..];
    strm[0] = entry_stream;
    strm[strm_off_flags] = secflag_alloc;
    strm[strm_off_nlen] = @intCast(nlen);
    wr16(set, entry_bytes + off_strm_hash, c.priv_exfat_name_hash(name.ptr, nlen));
    wr32(set, entry_bytes + off_strm_valid, cbytes);
    wr32(set, entry_bytes + strm_off_clus, first);
    wr32(set, entry_bytes + strm_off_dlen, cbytes);
    var n: u32 = 0;
    while (n < name_entries) : (n += 1) {
        const ne_base = (2 + n) * entry_bytes;
        set[ne_base] = entry_name;
        var ch: u32 = 0;
        while (ch < name_per_entry) : (ch += 1) {
            const pos = n * name_per_entry + ch;
            if (pos < nlen) {
                // A whole UTF-16 unit, not the low byte of one -- the same
                // correction the file-side builder needed.
                wr16(set, ne_base + name_off + @as(usize, ch) * 2, name[pos]);
            }
        }
    }
    wr16(set, off_file_csum, exfat_write.priv_exfat_set_checksum(set.ptr, total));
    return total;
}

/// Allocate and initialise one cluster as an empty exFAT directory.
///
/// Scans the allocation bitmap for a single free cluster, zeroes it, and only
/// then marks the bit. That order is deliberate: a failure while zeroing
/// leaves a cluster that is still free, so the volume loses nothing, whereas
/// marking first would leak it.
fn dirAlloc(m: *const Mount, out_clus: *u32, out_bmp: *u64) u16 {
    var bclus: u32 = 0;
    var blen: u32 = 0;
    var e = c.priv_exfat_find_bitmap(m, &bclus, &blen);
    if (e != ok) {
        return e;
    }
    out_bmp.* = c.priv_cluster_to_lba(m, bclus);
    e = c.priv_exfat_bitmap_scan(m, out_bmp.*, 1, out_clus);
    if (e != ok) {
        return e;
    }
    e = c.priv_exfat_zero_cluster(m, out_clus.*);
    if (e != ok) {
        return e;
    }
    e = c.priv_exfat_bitmap_mark(m, out_bmp.*, out_clus.*, 1);
    if (e != ok) {
        return e;
    }
    // Only now, with the bit actually set, is it true that the next free
    // cluster starts past this one.
    c.priv_alloc_hint_set(m, out_clus.* + 1);
    return ok;
}

/// Decide whether `leaf` may be created in `parent`: length first, then the
/// name lookup. Split out of `priv_exfat_mkdir` so that function stays inside
/// the function-size gate.
fn mkdirCheck(m: *const Mount, parent: *const c.exfat_dir_t, leaf: [*c]const u8, nlen: u32) u16 {
    if (nlen == 0) {
        return err_invalid_arg; // "/" or a trailing slash: the root itself
    }
    if (nlen > name_cap) {
        return err_invalid_arg;
    }
    var strm: [entry_bytes]u8 = @splat(0);
    var attr: u8 = 0;
    // mkdir REFUSES an existing name rather than replacing it, unlike
    // ra8_fs_write_file(), which is a create-or-replace verb. A directory and
    // a file are both an entry set under that name, so both refuse here.
    //
    // The third verdict is the one that is easy to drop: a BACKEND FAILURE is
    // not "the name is absent". Testing only for k_ra8_ok would turn an
    // unreadable directory into a green light to create, and the volume would
    // then hold two entry sets for one name -- the duplicate, reintroduced
    // through the error path instead of the happy one.
    const fe = c.priv_exfat_find(m, parent, leaf, &strm, &attr);
    if (fe == ok) {
        return err_exists;
    }
    if (fe != err_not_found) {
        return fe;
    }
    return ok;
}

/// Create a directory: resolve the parent, prove the leaf is free, reserve
/// the parent's slot run, allocate and zero the directory's cluster, then
/// link the entry set.
pub export fn priv_exfat_mkdir(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8) callconv(.c) u16 {
    var parent: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var leaf: [*c]const u8 = null;
    var e = priv_exfat_resolve_parent(m, path, &parent, &leaf);
    if (e != ok) {
        return e;
    }
    // The leaf becomes code units here, once, and every on-disk length below
    // counts those: the slot run, `NameLength`, and the name hash. Counting
    // the caller's BYTES instead is what made a directory whose name held one
    // multi-byte character unopenable by that name.
    var units: [name_cap]u16 = @splat(0);
    var nlen: u32 = 0;
    const ne = c.priv_exfat_name_to_units(m, leaf, &units, &nlen);
    if (ne == err_no_mem) {
        return err_invalid_arg; // over the name cap: an argument, not a full disk
    }
    if (ne != ok) {
        return ne;
    }
    e = mkdirCheck(m, &parent, leaf, nlen);
    if (e != ok) {
        return e;
    }
    // Reserve the parent's slot run BEFORE allocating, so a full parent
    // directory costs no bitmap churn and needs no rollback.
    const need = 2 + (nlen + name_per_entry - 1) / name_per_entry;
    var dclus: u32 = 0;
    var didx: u32 = 0;
    e = c.priv_exfat_find_dir_space(m, &parent, need, &dclus, &didx);
    if (e != ok) {
        return e;
    }
    var newclus: u32 = 0;
    var bmp_lba: u64 = 0;
    e = dirAlloc(m, &newclus, &bmp_lba);
    if (e != ok) {
        return e;
    }
    var set: [max_set_bytes]u8 = @splat(0);
    const bytes = buildDirSet(m, &set, units[0..nlen], nlen, newclus);
    e = c.priv_exfat_write_dir_set(m, dclus, didx, &set, bytes);
    if (e != ok) {
        _ = c.priv_exfat_bitmap_clear(m, bmp_lba, newclus, 1); // never leak the cluster
        return e;
    }
    return ok;
}

/// Decide whether an exFAT directory holds any live entry.
///
/// Walks the directory and stops at the first definite answer. Only two
/// things count as "not an occupant": the end-of-directory marker (0x00),
/// which ends the walk, and an entry whose in-use bit is clear, which is a
/// deleted remnant. Everything else -- a File entry, a Stream entry, a Name
/// entry, or any typed entry another implementation left -- is an occupant,
/// exactly as the FAT side's `priv_dir_is_empty` treats a live 8.3 entry.
///
/// exFAT has no "." / ".." entries, so unlike FAT there is nothing to
/// discount before the count starts: an empty exFAT directory really is an
/// empty cluster. The walk is bounded twice over: by the directory's own run
/// (or FAT chain) and by `k_exfat_scan_limit` (NASA Power of 10 Rule 2).
fn dirIsEmpty(m: *const Mount, dir: *const c.exfat_dir_t, out_empty: *u8) u16 {
    var cur: c.exfat_cursor_t = std.mem.zeroes(c.exfat_cursor_t);
    c.priv_exfat_cursor_init(dir, &cur);
    while (cur.scanned < scan_limit) {
        var e: [entry_bytes]u8 = @splat(0);
        const r = c.priv_exfat_next_entry(m, &cur, &e);
        if (r == err_not_found) {
            out_empty.* = 1; // the run ended without an occupant and without a 0x00
            return ok;
        }
        if (r != ok) {
            return r;
        }
        if (e[0] == entry_eod) {
            out_empty.* = 1;
            return ok;
        }
        if (e[0] & inuse_bit == 0) {
            // A deleted set's remnant is not an occupant, and neither are
            // its secondaries: the walk consumes the set whole (the same
            // SecondaryCount parse the name lookup does), or it would count
            // the Stream and Name slots of every deleted file as occupants
            // and refuse rmdir on directories that only hold dead files.
            if ((e[0] & ~inuse_bit) != (entry_file & ~inuse_bit)) {
                continue;
            }
            var skip: u32 = e[1];
            while (skip > 0) : (skip -= 1) {
                const s = c.priv_exfat_next_entry(m, &cur, &e);
                if (s == err_not_found) {
                    out_empty.* = 1;
                    return ok;
                }
                if (s != ok) {
                    return s;
                }
                if (e[0] == entry_eod) {
                    out_empty.* = 1;
                    return ok;
                }
            }
            continue;
        }
        out_empty.* = 0;
        return ok;
    }
    out_empty.* = 1;
    return ok;
}

/// Resolve a path to a removable exFAT directory's entry set: resolve the
/// parent, refuse the volume root, locate the leaf's entry set with
/// per-entry positions, and require it to carry the Directory attribute and
/// to name a real heap cluster. Split out of `priv_exfat_rmdir` so both stay
/// inside the function-size gate.
fn rmdirLocate(m: *const Mount, path: [*c]const u8, pos: [*c]c.exfat_setpos_t, out_count: *u32, strm: *[entry_bytes]u8, out_dir: *c.exfat_dir_t) u16 {
    var parent: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var leaf: [*c]const u8 = null;
    var e = priv_exfat_resolve_parent(m, path, &parent, &leaf);
    if (e != ok) {
        return e;
    }
    if (c.priv_strlen(leaf) == 0) {
        return err_invalid_arg; // "/" or a trailing slash: the root itself
    }
    var file_e: [entry_bytes]u8 = @splat(0);
    e = c.priv_exfat_find_set(m, &parent, leaf, pos, set_max, out_count, &file_e, strm);
    if (e != ok) {
        return e;
    }
    if (file_e[off_file_attr] & attr_directory == 0) {
        return err_invalid_arg; // a file: ra8_fs_unlink() is the verb for it
    }
    if (rd32(strm, strm_off_clus) < first_data) {
        return err_protocol; // a directory always owns a cluster
    }
    c.priv_exfat_dir_from_set(m, strm, out_dir);
    return ok;
}

/// Remove an exFAT directory: prove it is empty, then retire its entry set
/// and free its cluster, entries first and clusters second -- the same order
/// `priv_exfat_unlink` uses. A fault between the two leaves clusters marked
/// used with nothing pointing at them, which a host `fsck` reports and
/// repairs; the reverse order would leave a live entry set pointing at free
/// space, which it cannot.
pub export fn priv_exfat_rmdir(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8) callconv(.c) u16 {
    var pos: [set_max]c.exfat_setpos_t = @splat(std.mem.zeroes(c.exfat_setpos_t));
    var count: u32 = 0;
    var strm: [entry_bytes]u8 = @splat(0);
    var victim: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var e = rmdirLocate(m, path, &pos, &count, &strm, &victim);
    if (e != ok) {
        return e;
    }
    var empty: u8 = 0;
    e = dirIsEmpty(m, &victim, &empty);
    if (e != ok) {
        return e;
    }
    if (empty == 0) {
        return err_not_empty;
    }
    e = c.priv_exfat_drop_set(m, &pos, count);
    if (e != ok) {
        return e;
    }
    return c.priv_exfat_free_clusters(m, &strm);
}
