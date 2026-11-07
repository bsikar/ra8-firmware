//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT read side (RA8FW-1087): volume detect and parse, the up-case
//! verification walk, the directory cursor machinery, flat name matching,
//! and the read-side open, ported from ra8_fs_fat_exfat_read.c. The write
//! side lives in ra8_fs_exfat_openw_abi.zig; priv_exfat_open still routes
//! writing modes there.

const std = @import("std");
const c = @import("fs_c.zig").c;
const upcase = @import("ra8_fs_upcase_abi.zig");
const utf = @import("ra8_fs_utf_abi.zig");
const openw = @import("ra8_fs_exfat_openw_abi.zig");

pub const File = c.ra8_fs_file_t;
pub const Mount = c.ra8_fs_mount_t;

pub const ok: u16 = c.k_ra8_ok;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const err_not_supported: u16 = c.k_ra8_err_not_supported;
const err_not_found: u16 = c.k_ra8_err_not_found;

const entry_bytes: u32 = c.k_exfat_entry_bytes;
const name_cap: u32 = c.k_exfat_name_cap;
const name_per_entry: u32 = c.k_exfat_name_per_entry;
const scan_limit: u32 = c.k_exfat_scan_limit;

/// Byte-order readers the read path leans on; defined once in
/// ra8_fs_fat_prim_abi.zig.
extern fn priv_rd16(p: [*]const u8) u16;
extern fn priv_rd32(p: [*]const u8) u32;
extern fn priv_rd64(p: [*]const u8) u64;
extern fn priv_byte_copy(dst: [*]u8, src: [*]const u8, n: u32) void;
extern fn priv_cluster_bytes(m: *const Mount) u32;
extern fn priv_bps(m: *const Mount) u32;
extern fn priv_cluster_to_lba(m: *const Mount, cluster: u32) u64;
extern fn priv_fat_get(m: *const Mount, cluster: u32, out_value: *u32) u16;
extern fn priv_is_eoc(m: *const Mount, value: u32) u8;
extern fn priv_sec_io() [*]u8;
extern fn priv_read_sector(m: *const Mount, lba: u64, buf: [*]u8) u16;
extern fn priv_alloc_file_slot() ?*File;
extern fn priv_parse_bpb_into_mount(m: *Mount) u16;
extern fn priv_exfat_lookup(m: *const Mount, path: [*:0]const u8, out_strm: [*]u8, out_attr: *u8) u16;

/// The mount's shared sector scratch: the boot sector sits here when the
/// caller reaches priv_parse_volume.
extern var g_fs_scratch: [c.k_ra8_fs_sector_max]u8;

/// C strlen on a NUL-terminated name; shared with the FAT name code.
pub export fn priv_strlen(s: [*:0]const u8) callconv(.c) u32 {
    var n: u32 = 0;
    while (s[n] != 0) {
        n += 1;
    }
    return n;
}

/// ASCII case fold for 8.3 name comparisons; anything outside a-z is itself.
pub export fn priv_ascii_upper(ch: c_char) callconv(.c) c_char {
    if (ch >= 'a' and ch <= 'z') {
        return ch - ('a' - 'A');
    }
    return ch;
}

/// The volume label field spells "EXFAT   " on an exFAT boot sector.
fn isVolume(buf: [*]const u8) u8 {
    const sig = "EXFAT   ";
    var i: usize = 0;
    while (i < c.k_exfat_fsname_len) : (i += 1) {
        if (buf[@as(usize, @intCast(c.k_exfat_off_fsname)) + i] != sig[i]) {
            return 0;
        }
    }
    return 1;
}

/// Parse an exFAT VBR into the mount's geometry fields.
///
/// Maps exFAT's sector-relative region offsets + log2 geometry onto the
/// shared FAT fields so priv_cluster_to_lba / priv_fat_get (FAT32 path) serve
/// the data + FAT regions unchanged. BytesPerSectorShift 9..12 (512..4096
/// bytes) is accepted, and the shift must agree with the DEVICE's block size
/// seeded into `m.bytes_per_sector` at mount: a volume formatted for a
/// different sector size than the medium presents is unmountable, not
/// reinterpretable.
fn internalParse(m: *Mount, buf: [*]const u8) u16 {
    const shift = buf[@as(usize, @intCast(c.k_exfat_off_bps_shift))];
    if (shift < c.k_exfat_bps_shift_min or shift > c.k_exfat_bps_shift_max) {
        return err_not_supported;
    }
    if ((@as(u32, 1) << @intCast(shift)) != m.bytes_per_sector) {
        return err_not_supported;
    }
    m.type = c.k_ra8_fs_type_exfat;
    m.sectors_per_cluster = @as(u32, 1) << @intCast(buf[@as(usize, @intCast(c.k_exfat_off_spc_shift))]);
    m.num_fats = @as(u32, buf[@as(usize, @intCast(c.k_exfat_off_num_fats))]);
    m.fat_size_sectors = priv_rd32(buf + @as(usize, @intCast(c.k_exfat_off_fat_len)));
    m.first_fat_lba = priv_rd32(buf + @as(usize, @intCast(c.k_exfat_off_fat_lba)));
    m.first_data_lba = priv_rd32(buf + @as(usize, @intCast(c.k_exfat_off_heap_lba)));
    m.root_cluster = priv_rd32(buf + @as(usize, @intCast(c.k_exfat_off_root_clus)));
    m.count_of_clusters = priv_rd32(buf + @as(usize, @intCast(c.k_exfat_off_clus_count)));
    m.reserved_sectors = 0;
    m.root_entries = 0;
    m.first_root_lba = 0;
    m.total_sectors = priv_rd64(buf + @as(usize, @intCast(c.k_exfat_foff_vol_len)));
    // Last, because it walks the root directory and so needs every field
    // above. It cannot fail the mount: a volume it could not interrogate is
    // simply one whose fold this build will not vouch for.
    priv_exfat_upcase_verify(m);
    return ok;
}

/// Walk the root directory for the up-case-table entry and mark the mount
/// when its checksum is the one this build embeds.
pub export fn priv_exfat_upcase_verify(m: *Mount) callconv(.c) void {
    m.exfat_upcase_ok = 0;
    var cur: c.exfat_cursor_t = std.mem.zeroes(c.exfat_cursor_t);
    cur.cluster = m.root_cluster;
    while (cur.scanned < scan_limit) {
        var e: [entry_bytes]u8 = @splat(0);
        const r = priv_exfat_next_entry(m, &cur, &e);
        if (r != ok) {
            return;
        }
        if (e[0] == c.k_exfat_entry_eod) {
            return;
        }
        if (e[0] != c.k_exfat_entry_upcase) {
            continue;
        }
        if (priv_rd32(e[@as(usize, @intCast(c.k_exfat_upc_off_csum))..].ptr) == upcase.priv_exfat_upcase_checksum()) {
            m.exfat_upcase_ok = 1;
        }
        return;
    }
    // Falls out of the scan loop only after 65536 directory entries ahead of
    // the up-case one, which no volume this driver mounts has. The loop's own
    // exits (the up-case match and the EOD marker) are the real ends.
}

/// Dispatch on the boot sector still held in the shared scratch: an exFAT
/// VBR parses as exFAT, anything else as a FAT BPB.
pub export fn priv_parse_volume(m: *Mount) callconv(.c) u16 {
    if (isVolume(&g_fs_scratch) != 0) {
        return internalParse(m, &g_fs_scratch);
    }
    return priv_parse_bpb_into_mount(m);
}

/// Seed a directory cursor at the exFAT root.
pub export fn priv_exfat_dir_root(m: *const Mount, out: *c.exfat_dir_t) callconv(.c) void {
    out.cluster = m.root_cluster;
    // The root is always FAT-chained; the format has no other shape.
    out.contig_end = 0;
    // The root owns no File-entry set: its extent is the boot sector's FAT
    // chain, not a Stream entry, so priv_exfat_grow_dir extends the chain and
    // patches no metadata. 0 is the "no entry set" sentinel.
    out.self_cluster = 0;
    out.self_index = 0;
}

/// Seed a directory cursor from a subdirectory's Stream-extension entry.
pub export fn priv_exfat_dir_from_set(m: *const Mount, strm: [*]const u8, out: *c.exfat_dir_t) callconv(.c) void {
    out.cluster = priv_rd32(strm + @as(usize, @intCast(c.k_exfat_strm_off_clus)));
    out.contig_end = 0;
    if ((strm[@as(usize, @intCast(c.k_exfat_strm_off_flags))] & c.k_exfat_secflag_no_fat) == 0) {
        return;
    }
    const cbytes = priv_cluster_bytes(m);
    const size = priv_rd64(strm + @as(usize, @intCast(c.k_exfat_strm_off_dlen)));
    out.contig_end = out.cluster +% @as(u32, @truncate((size +% cbytes -% 1) / cbytes));
}

/// Start a cursor at a directory's first cluster.
pub export fn priv_exfat_cursor_init(dir: *const c.exfat_dir_t, out: *c.exfat_cursor_t) callconv(.c) void {
    out.cluster = dir.cluster;
    out.entry_in_cluster = 0;
    out.scanned = 0;
    out.contig_end = dir.contig_end;
}

/// Advance to the next cluster of a directory: adjacent while the run is
/// contiguous, else one FAT hop, with EOC ending the walk.
pub export fn priv_exfat_step_cluster(m: *const Mount, cluster: u32, contig_end: u32, out_next: *u32) callconv(.c) u16 {
    if (contig_end != 0) {
        const adjacent = cluster +% 1;
        if (adjacent >= contig_end) {
            return err_not_found;
        }
        out_next.* = adjacent;
        return ok;
    }
    var next: u32 = 0;
    const e = priv_fat_get(m, cluster, &next);
    if (e != ok) {
        return e;
    }
    if (priv_is_eoc(m, next) != 0) {
        return err_not_found;
    }
    out_next.* = next;
    return ok;
}

/// Read the 32-byte directory entry under a cursor and advance it.
pub export fn priv_exfat_next_entry(m: *const Mount, cur: *c.exfat_cursor_t, out: [*]u8) callconv(.c) u16 {
    const per_cluster = priv_cluster_bytes(m) / entry_bytes;
    if (cur.entry_in_cluster >= per_cluster) {
        var next: u32 = 0;
        const se = priv_exfat_step_cluster(m, cur.cluster, cur.contig_end, &next);
        if (se != ok) {
            return se;
        }
        cur.cluster = next;
        cur.entry_in_cluster = 0;
    }
    const byte_off = cur.entry_in_cluster * entry_bytes;
    const lba = priv_cluster_to_lba(m, cur.cluster) + byte_off / priv_bps(m);
    const sec = priv_sec_io();
    const e = priv_read_sector(m, lba, sec);
    if (e != ok) {
        return e;
    }
    priv_byte_copy(out, sec + @as(usize, @intCast(byte_off % priv_bps(m))), entry_bytes);
    cur.entry_in_cluster += 1;
    cur.scanned += 1;
    return ok;
}

/// Compare one file-name entry's 15 UTF-16 units with the needle at `pos`;
/// units past the needle end always agree, so a short name matches the tail
/// of its own entry set.
pub export fn priv_exfat_name_chunk_eq(entry: [*]const u8, name: [*]const u16, pos: u32, nlen: u32) callconv(.c) u8 {
    var i: u32 = 0;
    while (i < name_per_entry) : (i += 1) {
        if (pos + i >= nlen) {
            return 1;
        }
        const off = @as(usize, @intCast(c.k_exfat_name_off)) + @as(usize, i) * 2;
        const unit = priv_rd16(entry + off);
        if (upcase.priv_exfat_upcase_unit(unit) != upcase.priv_exfat_upcase_unit(name[pos + i])) {
            return 0;
        }
    }
    return 1;
}

/// Convert a path's flat name to UTF-16 units.
///
/// An ASCII name folds the same way under every conforming up-case table, so
/// it never depends on the volume carrying the one this build embeds.
/// Anything else does, and a NameHash computed with the wrong fold is a
/// disagreement with the on-disk format rather than a limitation of this API.
pub export fn priv_exfat_name_to_units(m: *const Mount, path: [*:0]const u8, out: [*]u16, out_units: *u32) callconv(.c) u16 {
    const err = utf.priv_utf8_to_utf16(path, out, name_cap, out_units);
    if (err != ok) {
        return err;
    }
    if (m.exfat_upcase_ok != 0) {
        return ok;
    }
    if (utf.priv_utf16_all_ascii(out, out_units.*) != 0) {
        return ok;
    }
    out_units.* = 0;
    return err_not_supported;
}

/// Convert a lookup name to units, folding the too-long failure into the
/// caller's own error code.
pub export fn priv_exfat_needle_units(m: *const Mount, name: [*:0]const u8, out: [*]u16, out_units: *u32, too_long: u16) callconv(.c) u16 {
    const e = priv_exfat_name_to_units(m, name, out, out_units);
    return if (e == err_no_mem) too_long else e;
}

/// Read a stream-ext + name set and test it against the needle.
///
/// Called with `cur` positioned just after a 0x85 File entry. Reads the 0xC0
/// stream-extension entry and the following 0xC1 name entries; on a full
/// match hands the WHOLE Stream entry back rather than a chosen few of its
/// fields. That is the cheaper contract for everyone: the callers want
/// different subsets of it -- `stat` the length and cluster, an open also
/// ValidDataLength and the flags -- and a copy of 32 bytes they already had
/// in hand costs less than another out-parameter each time one is needed.
/// Leftover name entries self-heal in priv_exfat_find.
fn matchSet(m: *const Mount, cur: *c.exfat_cursor_t, name: [*]const u16, nlen: u32, out_strm: [*]u8) u16 {
    var strm: [entry_bytes]u8 = @splat(0);
    var e = priv_exfat_next_entry(m, cur, &strm);
    if (e != ok) {
        return e;
    }
    if (strm[0] != c.k_exfat_entry_stream) {
        return err_not_found;
    }
    if (@as(u32, strm[@as(usize, @intCast(c.k_exfat_strm_off_nlen))]) != nlen) {
        return err_not_found;
    }
    var pos: u32 = 0;
    while (pos < nlen) : (pos += name_per_entry) {
        var nm: [entry_bytes]u8 = @splat(0);
        e = priv_exfat_next_entry(m, cur, &nm);
        if (e != ok) {
            return e;
        }
        if (nm[0] != c.k_exfat_entry_name) {
            return err_not_found;
        }
        if (priv_exfat_name_chunk_eq(&nm, name, pos, nlen) == 0) {
            return err_not_found;
        }
    }
    priv_byte_copy(out_strm, &strm, entry_bytes);
    return ok;
}

/// Scan a directory for a File entry whose set names `path`.
pub export fn priv_exfat_find(m: *const Mount, dir: *const c.exfat_dir_t, path_in: [*:0]const u8, out_strm: [*]u8, out_attr: *u8) callconv(.c) u16 {
    // Leading slashes are not part of the name; match FAT's priv_path_to_83
    // behavior so ra8_fs_open("/name") resolves on exFAT too.
    var path = path_in;
    while (path[0] == '/') {
        path += 1;
    }
    var need: [name_cap]u16 = @splat(0);
    var nlen: u32 = 0;
    const ne = priv_exfat_name_to_units(m, path, &need, &nlen);
    if (ne == err_no_mem) {
        // Longer than any name this volume can hold, so it is genuinely
        // absent -- a lookup says so rather than reporting a resource failure.
        return err_not_found;
    }
    if (ne != ok) {
        return ne;
    }
    var cur: c.exfat_cursor_t = std.mem.zeroes(c.exfat_cursor_t);
    priv_exfat_cursor_init(dir, &cur);
    while (cur.scanned < scan_limit) {
        var entry: [entry_bytes]u8 = @splat(0);
        var e = priv_exfat_next_entry(m, &cur, &entry);
        if (e != ok) {
            return e;
        }
        if (entry[0] == c.k_exfat_entry_eod) {
            return err_not_found;
        }
        if (entry[0] != c.k_exfat_entry_file) {
            continue;
        }
        e = matchSet(m, &cur, &need, nlen, out_strm);
        if (e == ok) {
            // FileAttributes is 16-bit but every bit we act on (directory,
            // archive) lives in the low byte, so the caller gets that byte.
            out_attr.* = entry[@as(usize, @intCast(c.k_exfat_off_file_attr))];
            return ok;
        }
        if (e != err_not_found) {
            return e;
        }
    }
    return err_not_found;
}

/// Populate a read handle from the file's Stream-extension entry.
///
/// Everything a READ needs and nothing a write does: the chain head, the two
/// lengths, and the contiguity flag. The streaming fields stay zeroed because
/// a read handle never grows the file -- but `valid_bytes` is not one of
/// them, because the bytes past it must read as zero (exFAT spec sec 7.4.5)
/// rather than as whatever the previous tenant of those clusters left.
fn seedRead(file: *File, handle: *Mount, strm: [*]const u8) void {
    const first = priv_rd32(strm + @as(usize, @intCast(c.k_exfat_strm_off_clus)));
    file.mount = handle;
    file.first_cluster = first;
    file.cur_cluster = first;
    file.walk_cache_idx = 0; // read accelerator seeded at the chain head
    file.walk_cache_cluster = first;
    file.size_bytes = priv_rd64(strm + @as(usize, @intCast(c.k_exfat_strm_off_dlen)));
    file.offset = 0;
    file.dir_entry_lba = 0;
    file.dir_entry_idx = 0;
    file.valid_bytes = priv_rd64(strm + @as(usize, @intCast(c.k_exfat_off_strm_valid)));
    file.entry_set_cluster = 0;
    file.entry_set_index = 0;
    file.entry_set_count = 0;
    file.alloc_clusters = 0;
    file.tail_cluster = 0;
    file.mode = c.k_ra8_fs_mode_read;
    file.no_fat_chain = if ((strm[@as(usize, @intCast(c.k_exfat_strm_off_flags))] & c.k_exfat_secflag_no_fat) != 0) 1 else 0;
    file.in_use = 1;
    file.dirty = 0;
}

/// Open a file for reading; writing modes route to the openw unit.
pub export fn priv_exfat_open(handle: *Mount, path: [*:0]const u8, mode: c.ra8_fs_mode_t, out_file: *?*File) callconv(.c) u16 {
    // Writing modes need the file's entry-set coordinates, its allocation and
    // its ValidDataLength -- none of which a read open has any use for -- so
    // they get their own path rather than a second half bolted onto this one.
    if (mode != c.k_ra8_fs_mode_read) {
        return openw.priv_exfat_open_write(handle, path, mode, out_file);
    }
    var strm: [entry_bytes]u8 = @splat(0);
    var attr: u8 = 0;
    const e = priv_exfat_lookup(handle, path, &strm, &attr);
    if (e != ok) {
        return e;
    }
    // A directory answers the name lookup exactly like a file and reports
    // DataLength 0, so without this it opens as a bogus empty file.
    if ((attr & c.k_exfat_attr_directory) != 0) {
        return err_invalid_arg;
    }
    const f = priv_alloc_file_slot() orelse return err_no_mem;
    seedRead(f, handle, &strm);
    out_file.* = f;
    return ok;
}
