//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! VFAT long-name layout helpers shared by the reader and the writer: the
//! 8.3 checksum that ties a chain to its short entry, laying one LFN slot
//! out, and reassembling a chain while a directory is scanned.
//!
//! Each LFN entry carries 13 UTF-16LE units at fixed offsets
//! (1/3/5/7/9, 14/16/18/20/22/24, 28/30). Reader and writer index the one
//! table below, so they cannot disagree about where a character lives.
//! The directory search that uses the reader (`priv_dir_find_long`) is
//! still in `ra8_fs_fat_lfn.c`.
//!
//! Bounded loops: every loop runs at most 13, 11 or `k_lfn_write_max` times.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const LfnState = c.lfn_state_t;

pub const chars_per_ent: u32 = c.k_lfn_chars_per_ent;
pub const write_max: u32 = c.k_lfn_write_max;
pub const max_entries: u32 = c.k_lfn_max_entries;
pub const seq_last: u8 = c.k_lfn_seq_last;
pub const unicode_pad: u16 = c.k_lfn_unicode_pad;
const seq_order_mask: u8 = c.k_lfn_seq_order_mask;
const off_seq: usize = c.k_lfn_off_seq;
const off_checksum: usize = c.k_lfn_off_checksum;
const off_attr: usize = c.k_dir_off_attr;
const name_len: usize = c.k_dir_name_field_len;
const dirent_len: usize = c.k_ra8_fs_dir_entry_bytes;
const attr_lfn: u8 = c.k_ra8_fs_attr_lfn;

/// Byte offset of each of the 13 UTF-16LE units inside an LFN entry.
pub const char_off = [13]u8{ 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

comptime {
    std.debug.assert(char_off.len == chars_per_ent);
}

pub export fn priv_sfn_checksum(name83: [*]const u8) callconv(.c) u8 {
    var sum: u8 = 0;
    for (name83[0..name_len]) |ch| sum = ((sum & 1) << 7) +% (sum >> 1) +% ch;
    return sum;
}

/// Lay out slot `order` (1-based) of the chain for `name[0..nlen]`: the
/// name's units, one NUL right after the last one, 0xFFFF padding past it.
pub export fn priv_lfn_fill_slot(
    ent: [*]u8,
    name: [*c]const u16,
    nlen: u32,
    order: u32,
    is_last: u8,
    csum: u8,
) callconv(.c) void {
    @memset(ent[0..dirent_len], 0);
    ent[off_seq] = @truncate(order | if (is_last != 0) @as(u32, seq_last) else 0);
    ent[off_attr] = attr_lfn;
    ent[off_checksum] = csum;
    const base = (order - 1) * chars_per_ent;
    for (char_off, 0..) |off, i| {
        const pos = base + @as(u32, @intCast(i));
        const val: u16 = if (pos < nlen) name[pos] else if (pos == nlen) 0 else unicode_pad;
        std.mem.writeInt(u16, ent[off..][0..2], val, .little);
    }
}

pub export fn priv_lfn_reset(s: *LfnState) callconv(.c) void {
    @memset(&s.units, 0);
    s.checksum = 0;
    s.have = 0;
}

/// Fold one LFN entry into the chain. An order outside 1..k_lfn_max_entries
/// means a corrupt chain, so that entry is ignored.
pub export fn priv_lfn_add(s: *LfnState, ent: [*]const u8) callconv(.c) void {
    const order: u32 = ent[off_seq] & seq_order_mask;
    if (order < 1 or order > max_entries) return;
    s.checksum = ent[off_checksum];
    s.have = 1;
    const base = (order - 1) * chars_per_ent;
    for (char_off, 0..) |off, i| {
        const pos = base + @as(u32, @intCast(i));
        const val = std.mem.readInt(u16, ent[off..][0..2], .little);
        if (val == 0 or val == unicode_pad) {
            s.units[pos] = 0; // terminator or padding ends this group
            break;
        }
        s.units[pos] = val;
    }
}

/// The reassembled name for the 8.3 entry `name83`, or null when no chain
/// was collected or its checksum does not bind it to this entry.
pub export fn priv_lfn_units_for(s: *const LfnState, name83: [*]const u8, out_units: *u32) callconv(.c) ?[*]const u16 {
    out_units.* = 0;
    if (s.have == 0 or s.units[0] == 0) return null;
    if (s.checksum != priv_sfn_checksum(name83)) return null;
    out_units.* = @intCast(std.mem.indexOfScalar(u16, &s.units, 0) orelse s.units.len);
    return &s.units;
}

fn unit_in_set(u: u16, set: []const u8) u8 {
    for (set) |ch| {
        if (u == ch) return 1;
    }
    return 0;
}

/// Is `u` legal in a VFAT long name? The long name inherits the short name's
/// illegal set minus the characters VFAT deliberately allows (`+ , ; = [ ]`
/// and space); control characters and DEL are refused, and everything above
/// ASCII is allowed: the slots store UTF-16 and the reader reads them back
/// as such, so there is no code-page cut here.
pub export fn internal_unit_is_lfn_legal(u: u16) callconv(.c) u8 {
    const v: u32 = u;
    if (v < 0x20) return 0; // k_lfn_space: control characters, including the NUL that ends the name
    if (v == 0x7F) return 0; // k_lfn_del: DEL is a control code wherever it appears
    if (unit_in_set(u, "\"*/:<>?\\|") != 0) return 0;
    return 1;
}

/// Is `u` usable verbatim inside a packed 8.3 short name? Stricter than the
/// long-name rule by the extra short-name exclusions `+ , ; = [ ]` and space,
/// by the dot -- which in an 8.3 name is a field separator rather than a
/// character -- and by the ASCII cut, which belongs here rather than there:
/// a packed 8.3 field is an OEM code page byte and this adapter only writes
/// ASCII into one.
pub export fn internal_unit_is_83_legal(u: u16) callconv(.c) u8 {
    if (u > 0x7F) return 0; // k_lfn_del: a unit above the ASCII cut needs a long-name chain
    if (u == '.') return 0;
    if (unit_in_set(u, "+,;=[] ") != 0) return 0;
    return 1;
}

fn name_dot_index(leaf: [*c]const u16, n: u32) u32 {
    var dot: u32 = n;
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        if (leaf[i] == '.') dot = i;
    }
    return dot;
}

fn case_observe(leaf: [*c]const u16, from: u32, to: u32, seen: *u8) void {
    var i: u32 = from;
    while (i < to) : (i += 1) {
        const ch: u32 = leaf[i];
        if (ch >= 'A' and ch <= 'Z') {
            seen.* |= 0x01; // k_case_seen_upper (ra8_fs_name_local_t, fat_name.c-local)
        } else if (ch >= 'a' and ch <= 'z') {
            seen.* |= 0x02; // k_case_seen_lower
        }
        // Digits and punctuation carry no case; they constrain nothing.
    }
}

/// Upper-case one ASCII character. Characters outside 'a'..'z' return
/// unchanged; the caller's cast decides how bytes above DEL got here.
pub export fn priv_to_upper(ch: u8) callconv(.c) u8 {
    if (ch >= 'a' and ch <= 'z') {
        return ch - 'a' + 'A';
    }
    return ch;
}

/// Can `leaf` be stored as a packed 8.3 name without losing anything? Requires
/// a 1..8 character base, a 0 or 1..3 character extension, and every character
/// legal in a short name. A second dot is caught by that last rule rather than
/// by a count of its own: everything before the LAST dot is the base, so an
/// earlier one is simply a character the base may not hold. A trailing dot
/// fails the extension test and is therefore stored as a long name.
pub export fn internal_name_is_83(leaf: [*c]const u16, n: u32) callconv(.c) u8 {
    const dot = name_dot_index(leaf, n);
    const base_len = dot;
    if (base_len == 0 or base_len > c.k_filename_base_len) return 0;
    if (dot < n) {
        const ext_len = (n - dot) - 1;
        if (ext_len == 0 or ext_len > c.k_filename_ext_len) return 0;
    }
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        if (i == dot) continue;
        if (internal_unit_is_83_legal(leaf[i]) == 0) return 0;
    }
    return 1;
}

/// Decide the case shape of an 8.3-representable leaf. A half of the name that
/// is all one case round-trips through a `DIR_NTRes` bit; a half that mixes
/// cases does not, and the whole name then needs a long-name chain. The two
/// halves are judged independently, exactly as the two flag bits are.
/// Preconditions: `leaf` packs to 8.3 (internal_name_is_83 returned 1).
pub export fn internal_name_case_kind(leaf: [*c]const u16, n: u32, out_ntres: [*c]u8) callconv(.c) c.ra8_fs_name_kind_t {
    const dot = name_dot_index(leaf, n);
    var base_seen: u8 = 0;
    var ext_seen: u8 = 0;
    case_observe(leaf, 0, dot, &base_seen);
    if (dot < n) case_observe(leaf, dot + 1, n, &ext_seen);
    out_ntres.* = 0;
    if (base_seen == 0x03) return c.k_name_kind_long; // k_case_seen_mixed
    if (ext_seen == 0x03) return c.k_name_kind_long;
    if (base_seen == 0x02) out_ntres.* |= @as(u8, c.k_ntres_base_lower); // k_case_seen_lower
    if (ext_seen == 0x02) out_ntres.* |= @as(u8, c.k_ntres_ext_lower);
    return c.k_name_kind_short;
}

/// Pack the base portion of a path into out11[0..7]. Reads characters from the
/// cursor at `path_io` up to a `.` or NUL, upper-cases them, and writes them
/// into out11[0..7], advancing the cursor to the `.` or terminator on success.
/// Returns 0 on overflow or an empty base. The caller pre-pads out11 with
/// spaces.
pub export fn internal_pack_base(path_io: [*c][*c]const u8, out11: [*c]u8) callconv(.c) u8 {
    var path = path_io.*;
    var base_len: u8 = 0;
    while (path[0] != 0 and path[0] != '.') {
        if (base_len >= c.k_filename_base_len) return 0;
        if (path[0] > 0x7F) return 0; // k_lfn_del: a packed 8.3 name here is ASCII; a UTF-8 byte is not one
        out11[base_len] = priv_to_upper(path[0]);
        base_len += 1;
        path += 1;
    }
    if (base_len == 0) return 0;
    path_io.* = path;
    return 1;
}

/// Pack the extension portion of a path into out11[8..10]. If the cursor is
/// not at a `.`, returns success with no writes. Returns 0 on overflow or a
/// byte above the ASCII cut. The caller pre-pads out11 with spaces.
pub export fn internal_pack_ext(path: [*c]const u8, out11: [*c]u8) callconv(.c) u8 {
    if (path[0] != '.') return 1;
    var p = path + 1;
    var ext_len: u32 = 0;
    while (p[0] != 0) {
        if (ext_len >= c.k_filename_ext_len) return 0;
        if (p[0] > 0x7F) return 0; // as in the base: an 8.3 field never holds a UTF-8 byte
        out11[@as(u32, c.k_filename_base_len) + ext_len] = priv_to_upper(p[0]);
        ext_len += 1;
        p += 1;
    }
    return 1;
}

/// Convert "FILE.TXT" (a caller-supplied path) to the packed 11-byte 8.3
/// form, space-padded as on disk. Lower-case input is upper-cased. Returns 0
/// on a bad name (base over 8, extension over 3, missing characters), 1 on
/// success. No kanji escape on the way in: both packers refuse every byte
/// above DEL, so no input can produce a packed 0xE5 first byte. The escape
/// still matters on the way out, where priv_83_to_str restores it for a
/// volume some Shift-JIS system wrote.
pub export fn priv_path_to_83(path: [*c]const u8, out11: [*c]u8) callconv(.c) u8 {
    if (path == null or out11 == null) return 0;
    var p = path;
    while (p[0] == '/') p += 1;
    var i: u32 = 0;
    while (i < c.k_max_8_3_name) : (i += 1) out11[i] = ' ';
    if (internal_pack_base(&p, out11) == 0) return 0;
    if (internal_pack_ext(p, out11) == 0) return 0;
    return 1;
}
