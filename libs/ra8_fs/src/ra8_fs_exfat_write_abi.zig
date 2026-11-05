//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT write-side primitives: the allocation bitmap (locate, probe, scan,
//! mark) and the File + Stream + Name entry-set factory.
//!
//! Port of `ra8_fs_fat_exfat_write.c`. The pairing is the write side's mirror
//! of the read module: before any exFAT write can place a byte it needs two
//! things, somewhere to put it and something to name it. The bitmap half finds
//! free clusters against the volume's allocation bitmap; the set half builds
//! the typed entry triple, checksums it and writes it into directory space.
//! The streaming engine that drives both stays in `ra8_fs_fat_exfat_stream.c`,
//! reached through `fs_c.zig`.
//!
//! The checksum is exFAT's rotate-right-then-add 16-bit fold. It serves both
//! the directory SetChecksum and the name hash, and the name hash folds the
//! UP-CASED name one little-endian byte at a time: an ASCII-only up-case rule
//! would store a hash no compliant reader recomputes for any name outside
//! ASCII, so a host could list a file and then not find it.

const std = @import("std");
const c = @import("fs_c.zig").c;
const upcase = @import("ra8_fs_upcase_abi.zig");

pub const Mount = c.ra8_fs_mount_t;

pub const ok: u16 = c.k_ra8_ok;

const entry_bytes: usize = @intCast(c.k_exfat_entry_bytes);
const name_per_entry: u32 = @intCast(c.k_exfat_name_per_entry);
const name_off: usize = @intCast(c.k_exfat_name_off);
const scan_limit: u32 = @intCast(c.k_exfat_scan_limit);
const csum_hi_bit: u16 = @intCast(c.k_exfat_csum_hi_bit);
const off_file_secnt: usize = @intCast(c.k_exfat_off_file_secnt);
const off_file_csum: usize = @intCast(c.k_exfat_off_file_csum);
const off_file_attr: usize = @intCast(c.k_exfat_off_file_attr);
const off_strm_hash: usize = @intCast(c.k_exfat_off_strm_hash);
const strm_off_flags: usize = @intCast(c.k_exfat_strm_off_flags);
const strm_off_nlen: usize = @intCast(c.k_exfat_strm_off_nlen);
const strm_off_clus: usize = @intCast(c.k_exfat_strm_off_clus);
const strm_off_dlen: usize = @intCast(c.k_exfat_strm_off_dlen);
const entry_eod: u8 = @intCast(c.k_exfat_entry_eod);
const entry_bitmap: u8 = @intCast(c.k_exfat_entry_bitmap);
const entry_file: u8 = @intCast(c.k_exfat_entry_file);
const entry_stream: u8 = @intCast(c.k_exfat_entry_stream);
const entry_name: u8 = @intCast(c.k_exfat_entry_name);
const secflag_poss: u8 = @intCast(c.k_exfat_secflag_poss);
const attr_archive: u16 = @intCast(c.k_exfat_attr_archive);
const bit_mask: u32 = @intCast(c.k_exfat_bit_mask);
const bit_shift: u5 = @intCast(c.k_exfat_bit_shift);
const inuse_bit: u8 = @intCast(c.k_exfat_inuse_bit);
const max_set_bytes: usize = @intCast(c.k_exfat_max_set_bytes);
const dir_grow_max: u32 = @intCast(c.k_exfat_dir_grow_max);
const first_data: u32 = @intCast(c.k_cluster_first_data);
const utf_byte_mask: u16 = @intCast(c.k_utf_byte_mask);
const utf_byte_shift: u5 = @intCast(c.k_utf_byte_shift);

const err_not_found: u16 = c.k_ra8_err_not_found;
const err_no_mem: u16 = c.k_ra8_err_no_mem;

/// One step of exFAT's rotate-right-then-add 16-bit checksum.
fn csumAdd(cs: u16, b: u8) u16 {
    const hi: u16 = if ((cs & 1) != 0) csum_hi_bit else 0;
    return hi +% (cs >> 1) +% @as(u16, b);
}

/// The name hash lives in its own unit now (RA8FW-1115); aliased back so
/// the write side keeps its API.
pub const priv_exfat_name_hash = @import("ra8_fs_exfat_hash_abi.zig").priv_exfat_name_hash;

/// SetChecksum over `bytes` of an entry set, skipping the two checksum bytes
/// themselves.
pub export fn priv_exfat_set_checksum(set: [*c]const u8, bytes: u32) callconv(.c) u16 {
    var cs: u16 = 0;
    var i: u32 = 0;
    while (i < bytes) : (i += 1) {
        if (i == off_file_csum or i == off_file_csum + 1) {
            continue;
        }
        cs = csumAdd(cs, set[i]);
    }
    return cs;
}

/// Locate the allocation bitmap: the one exFAT scan that is not
/// parameterised by a directory, because the bitmap entry lives in the ROOT
/// directory by definition.
pub export fn priv_exfat_find_bitmap(m: ?*const Mount, out_clus: ?*u32, out_len: ?*u32) callconv(.c) u16 {
    var root: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    c.priv_exfat_dir_root(m, &root);
    var cur: c.exfat_cursor_t = std.mem.zeroes(c.exfat_cursor_t);
    c.priv_exfat_cursor_init(&root, &cur);
    var e: [entry_bytes]u8 = @splat(0);
    while (cur.scanned < scan_limit) {
        const r: u16 = c.priv_exfat_next_entry(m, &cur, &e);
        if (r != ok) {
            return r;
        }
        if (e[0] == entry_eod) {
            return err_not_found;
        }
        if (e[0] == entry_bitmap) {
            out_clus.?.* = c.priv_rd32(&e[strm_off_clus]);
            out_len.?.* = c.priv_rd32(&e[strm_off_dlen]);
            return ok;
        }
    }
    return err_not_found; // 65536 entries required
}

/// Scan a window of the allocation bitmap for a contiguous free run of
/// `need` clusters, loading each bitmap sector once.
fn bitmapWindow(m: ?*const Mount, bmp_lba: u64, from: u32, need: u32, out_clus: *u32) u16 {
    var run: u32 = 0;
    var start: u32 = 0;
    var loaded: u64 = std.math.maxInt(u64);
    const sec: [*]u8 = c.priv_sec_io();
    var idx: u32 = from;
    while (idx < m.?.count_of_clusters) : (idx += 1) {
        const lba = bmp_lba + (@as(u64, idx >> bit_shift) / c.priv_bps(m));
        if (lba != loaded) {
            const e = c.priv_read_sector(m, lba, sec);
            if (e != ok) {
                return e;
            }
            loaded = lba;
        }
        const byte = (idx >> bit_shift) % c.priv_bps(m);
        const bit: u5 = @intCast(idx & bit_mask);
        if (((@as(u32, sec[byte]) >> bit) & 1) != 0) {
            run = 0;
            continue;
        }
        if (run == 0) {
            start = idx;
        }
        run += 1;
        if (run >= need) {
            out_clus.* = start + first_data;
            return ok;
        }
    }
    return err_no_mem;
}

/// Hinted pass, then a full rescan from cluster index 0.
pub export fn priv_exfat_bitmap_scan(m: ?*const Mount, bmp_lba: u64, need: u32, out_clus: ?*u32) callconv(.c) u16 {
    const hint = c.priv_alloc_hint_get(m);
    var from: u32 = 0;
    if (hint > first_data) {
        from = hint - first_data;
    }
    if (from >= m.?.count_of_clusters) {
        from = 0;
    }
    const e = bitmapWindow(m, bmp_lba, from, need, out_clus.?);
    if (e != err_no_mem) {
        return e;
    }
    if (from == 0) {
        return e;
    }
    return bitmapWindow(m, bmp_lba, 0, need, out_clus.?);
}

/// Keep one bitmap sector resident: write the previous one back, read the new
/// one. `loaded` starts at its max value, which never names a real LBA.
pub export fn priv_exfat_bmp_switch(m: ?*const Mount, lba: u64, loaded: *u64, sec: [*]u8) callconv(.c) u16 {
    if (lba == loaded.*) {
        return ok;
    }
    if (loaded.* != std.math.maxInt(u64)) {
        const we = c.priv_write_sector(m, loaded.*, sec);
        if (we != ok) {
            return we;
        }
    }
    const e = c.priv_read_sector(m, lba, sec);
    if (e != ok) {
        return e;
    }
    loaded.* = lba;
    return ok;
}

/// Mark `count` clusters allocated in the bitmap, one read-modify-write per
/// sector touched, with a single write-back at the end.
pub export fn priv_exfat_bitmap_mark(m: ?*const Mount, bmp_lba: u64, clus: u32, count: u32) callconv(.c) u16 {
    var loaded: u64 = std.math.maxInt(u64);
    const sec: [*]u8 = c.priv_sec_io();
    var k: u32 = 0;
    while (k < count) : (k += 1) {
        const idx = (clus - first_data) + k;
        const lba = bmp_lba + (@as(u64, idx >> bit_shift) / c.priv_bps(m));
        const byte = (idx >> bit_shift) % c.priv_bps(m);
        const bit: u5 = @intCast(idx & bit_mask);
        const e = priv_exfat_bmp_switch(m, lba, &loaded, sec);
        if (e != ok) {
            return e;
        }
        sec[byte] |= (@as(u8, 1) << @as(u3, @truncate(bit)));
    }
    return c.priv_write_sector(m, loaded, sec);
}

/// One sector read, one bit: is cluster `clus` free?
pub export fn priv_exfat_bitmap_test(m: ?*const Mount, bmp_lba: u64, clus: u32, out_free: ?*u8) callconv(.c) u16 {
    const idx = clus - first_data;
    const lba = bmp_lba + (@as(u64, idx >> bit_shift) / c.priv_bps(m));
    const byte = (idx >> bit_shift) % c.priv_bps(m);
    const bit: u5 = @intCast(idx & bit_mask);

    const sec: [*]u8 = c.priv_sec_io();
    const e = c.priv_read_sector(m, lba, sec);
    if (e != ok) {
        return e;
    }
    const busy = ((@as(u32, sec[byte]) >> bit) & 1) != 0;
    out_free.?.* = if (busy) 0 else 1;
    return ok;
}

/// Read one 32-byte directory entry by index within a cluster.
fn readEntry(m: ?*const Mount, cluster: u32, idx: u32, out: *[entry_bytes]u8) u16 {
    const byte_off = @as(u64, idx) * entry_bytes;
    const lba = c.priv_cluster_to_lba(m, cluster) + (byte_off / c.priv_bps(m));
    const sec: [*]u8 = c.priv_sec_io();
    const e = c.priv_read_sector(m, lba, sec);
    if (e != ok) {
        return e;
    }
    const off: usize = @intCast(byte_off % c.priv_bps(m));
    @memcpy(out, sec[off .. off + entry_bytes]);
    return ok;
}

/// True when a directory entry slot is free: the end-of-directory marker, or
/// bit 7 clear. exFAT marks an in-use entry with bit 7 set.
fn slotFree(type_byte: u8) bool {
    if (type_byte == entry_eod) {
        return true;
    }
    return (type_byte & inuse_bit) == 0;
}

/// Scan one directory cluster for `need` consecutive free entries. A set is
/// never split across two clusters.
fn spaceInCluster(m: ?*const Mount, cluster: u32, need: u32, out_idx: *u32) u16 {
    const per_cluster = c.priv_cluster_bytes(m) / @as(u32, @intCast(entry_bytes));
    var run: u32 = 0;
    var i: u32 = 0;
    while (i < per_cluster) : (i += 1) {
        var e: [entry_bytes]u8 = @splat(0);
        const r = readEntry(m, cluster, i, &e);
        if (r != ok) {
            return r;
        }
        if (!slotFree(e[0])) {
            run = 0;
            continue;
        }
        if (run == 0) {
            out_idx.* = i;
        }
        run += 1;
        if (run >= need) {
            return ok;
        }
    }
    return err_no_mem;
}

/// Scan a directory's existing clusters for `need` consecutive free slots.
/// Reports no_mem when the CURRENT run has no room; growing the directory is
/// the caller's job. Bounded by the entry scan limit.
fn scanDirSpace(m: ?*const Mount, dir: *const c.exfat_dir_t, need: u32, out_clus: *u32, out_idx: *u32) u16 {
    var cluster = dir.cluster;
    var guard: u32 = 0;
    while (guard < scan_limit) : (guard += 1) {
        const r = spaceInCluster(m, cluster, need, out_idx);
        if (r == ok) {
            out_clus.* = cluster;
            return ok;
        }
        if (r != err_no_mem) {
            return r;
        }
        var next: u32 = 0;
        const fe = c.priv_exfat_step_cluster(m, cluster, dir.contig_end, &next);
        if (fe == err_not_found) {
            return err_no_mem; // the current run is exhausted -- the caller grows
        }
        if (fe != ok) {
            return fe;
        }
        cluster = next;
    }
    return err_no_mem; // 65536 directory clusters required
}

/// Find `need` consecutive free entry slots for a directory set, growing the
/// directory up to the one-grow bound when the current run has no room. A
/// mutable copy of `dir` is walked because growing advances `contig_end` (and
/// drops it to 0 when the run converts to a FAT chain), so the rescan reaches
/// the cluster the grow just appended.
pub export fn priv_exfat_find_dir_space(m: ?*const Mount, dir: ?*const c.exfat_dir_t, need: u32, out_clus: ?*u32, out_idx: ?*u32) callconv(.c) u16 {
    var work = dir.?.*;
    var grow: u32 = 0;
    while (grow <= dir_grow_max) : (grow += 1) {
        const e = scanDirSpace(m, &work, need, out_clus.?, out_idx.?);
        if (e != err_no_mem) {
            return e; // found a run, or a hard read error
        }
        if (grow == dir_grow_max) {
            break; // one grow already yields more than any set needs; stop
        }
        const ge = c.priv_exfat_grow_dir(m, &work);
        if (ge != ok) {
            return ge; // the volume, not the directory, is out of room
        }
    }
    return err_no_mem;
}

/// Build a zero-length File + Stream + Name entry set: FirstCluster 0,
/// DataLength 0, ValidDataLength 0, GeneralSecondaryFlags = AllocationPossible
/// with NoFatChain CLEAR. That is what exFAT spec sec 7.4.4 requires of an
/// empty file, and the first flush after a byte lands rewrites the three
/// fields and sets NoFatChain if the run is still contiguous. Returns the
/// total byte length of the built set.
fn buildSet(set: *[max_set_bytes]u8, name: [*c]const u16, nlen: u32) u32 {
    const name_entries = (nlen + name_per_entry - 1) / name_per_entry;
    const sec_count = 1 + name_entries;
    const total = (1 + sec_count) * @as(u32, @intCast(entry_bytes));
    @memset(set[0..total], 0);
    set[0] = entry_file;
    set[off_file_secnt] = @truncate(sec_count);
    c.priv_wr16(&set[off_file_attr], attr_archive);
    // exFAT's stamps are zero-is-illegal like FAT's -- month and day are
    // 1-based -- plus the 10 ms increments and the UtcOffset bytes. Stamped
    // before the SetChecksum below, which covers these bytes.
    c.priv_exfat_file_stamp_create(set);
    const strm = set[entry_bytes..];
    strm[0] = entry_stream;
    strm[strm_off_flags] = secflag_poss;
    strm[strm_off_nlen] = @truncate(nlen);
    c.priv_wr16(&strm[off_strm_hash], priv_exfat_name_hash(name, nlen));
    var n: u32 = 0;
    while (n < name_entries) : (n += 1) {
        const ne = set[(2 + n) * entry_bytes ..];
        ne[0] = entry_name;
        var ch: u32 = 0;
        while (ch < name_per_entry) : (ch += 1) {
            const pos = (n * name_per_entry) + ch;
            if (pos < nlen) {
                // A whole UTF-16 unit, not the low byte of one: one unit per
                // INPUT BYTE turned every multi-byte character into that many
                // garbage characters and made NameLength count bytes where
                // the format counts units.
                c.priv_wr16(&ne[name_off + ch * 2], name[pos]);
            }
        }
    }
    c.priv_wr16(&set[off_file_csum], priv_exfat_set_checksum(set, total));
    return total;
}

/// Write `bytes` of an entry set into a directory, entry by entry, one
/// read-modify-write per 32-byte slot.
pub export fn priv_exfat_write_dir_set(m: ?*const Mount, cluster: u32, idx: u32, set: [*c]const u8, bytes: u32) callconv(.c) u16 {
    const count = bytes / @as(u32, @intCast(entry_bytes));
    var k: u32 = 0;
    while (k < count) : (k += 1) {
        const byte_off = @as(u64, idx + k) * entry_bytes;
        const lba = c.priv_cluster_to_lba(m, cluster) + (byte_off / c.priv_bps(m));
        const sec: [*]u8 = c.priv_sec_io();
        var e = c.priv_read_sector(m, lba, sec);
        if (e != ok) {
            return e;
        }
        const off: usize = @intCast(byte_off % c.priv_bps(m));
        const src: [*]const u8 = set + @as(usize, k) * entry_bytes;
        @memcpy(sec[off .. off + entry_bytes], src[0..entry_bytes]);
        e = c.priv_write_sector(m, lba, sec);
        if (e != ok) {
            return e;
        }
    }
    return ok;
}

/// One directory-slot scan, one set write: the whole link step.
pub export fn priv_exfat_link(m: ?*const Mount, dir: ?*const c.exfat_dir_t, name: [*c]const u16, nlen: u32, out_head: ?*c.exfat_setpos_t, out_count: ?*u32) callconv(.c) u16 {
    const name_entries = (nlen + name_per_entry - 1) / name_per_entry;
    const need = 2 + name_entries;
    var dclus: u32 = 0;
    var didx: u32 = 0;
    var e = priv_exfat_find_dir_space(m, dir, need, &dclus, &didx);
    if (e != ok) {
        return e;
    }
    var set: [max_set_bytes]u8 = @splat(0);
    const bytes = buildSet(&set, name, nlen);
    e = priv_exfat_write_dir_set(m, dclus, didx, &set, bytes);
    if (e != ok) {
        return e;
    }
    out_head.?.cluster = dclus;
    out_head.?.index = didx;
    out_count.?.* = need;
    return ok;
}
