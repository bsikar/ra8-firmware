//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the directory write-path helpers
//! (`ra8_fs_dir_write_abi.zig`): alias probing stops at the first basis
//! with no directory entry, propagates backend errors, and honours the
//! Rule 2 tail bound; the slot cursor advances inside a sector without
//! touching the disk and rolls over with one write-back, one walk step,
//! and one read, answering no_mem when the walk runs off the directory.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");
const write = fs.dir_write;

const ok: u16 = c.k_ra8_ok;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const err_protocol: u16 = c.k_ra8_err_protocol_error;
const entry_bytes: u32 = c.k_ra8_fs_dir_entry_bytes;

fn setup() *const c.ra8_fs_mount_t {
    fake.reset(c.k_ra8_fs_type_fat16);
    fake.dir_lba = fake.walk_lba;
    fake.walk_fixed = 2;
    return fake.mount();
}

test "alias probing stops at the first basis with no entry" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var out11: [entry_bytes]u8 = @splat(0);
    fake.find83_not_found = true;
    try std.testing.expectEqual(ok, write.internal_alias_unique(m, &loc, null, 4, &out11));
}

test "alias probing propagates a backend error" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var out11: [entry_bytes]u8 = @splat(0);
    fake.find83_err = err_protocol;
    try std.testing.expectEqual(err_protocol, write.internal_alias_unique(m, &loc, null, 4, &out11));
}

test "the slot cursor advances inside the sector without touching the disk" {
    const m = setup();
    var cur: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    var buf: [512]u8 = @splat(0);
    var lba: u64 = fake.dir_lba;
    fake.sector_written = null;
    cur.ent = 5;
    try std.testing.expectEqual(ok, write.internal_slot_advance(m, &cur, &buf, &lba));
    try std.testing.expectEqual(@as(u32, 6), cur.ent);
    try std.testing.expect(fake.sector_written == null);
}

test "the slot cursor rolls over with one write, one walk step, one read" {
    const m = setup();
    var cur: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    var buf: [512]u8 = @splat(0);
    var lba: u64 = fake.dir_lba;
    fake.sector_written = null;
    cur.ent = 15;
    cur.w.fixed_remaining = 2;
    cur.w.cur_lba = fake.dir_lba;
    try std.testing.expectEqual(ok, write.internal_slot_advance(m, &cur, &buf, &lba));
    try std.testing.expectEqual(@as(u32, 0), cur.ent);
    try std.testing.expectEqual(fake.dir_lba, fake.sector_written.?);
    try std.testing.expectEqual(fake.dir_lba + 1, lba);
}

test "a rollover off the directory's end answers no_mem" {
    const m = setup();
    var cur: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    var buf: [512]u8 = @splat(0);
    var lba: u64 = fake.dir_lba;
    cur.ent = 15;
    cur.w.fixed_remaining = 0;
    cur.w.cur_lba = fake.dir_lba;
    try std.testing.expectEqual(err_no_mem, write.internal_slot_advance(m, &cur, &buf, &lba));
}

test "a FAT12/16 fixed root refuses to grow" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    try std.testing.expectEqual(err_no_mem, write.internal_dir_grow(m, &loc));
}

test "grow appends one zeroed cluster at the chain's end" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    try std.testing.expectEqual(ok, write.internal_dir_grow(m, &loc));
    // The zero write and the FAT link write both ran; sector_written holds the last one (the FAT write).
    try std.testing.expect(fake.sector_written != null);
    var linked = false;
    for (fake.fat) |v| {
        if (v == 6) linked = true;
    }
    try std.testing.expect(linked);
}

test "a failed FAT link leaves the returned error intact" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    fake.fat_set_err = err_no_mem;
    try std.testing.expectEqual(err_no_mem, write.internal_dir_grow(m, &loc));
}

export fn priv_alloc_eoc_cluster(m: [*c]const u8, out_c: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_c.* = 6;
    return 0;
}

test "LFN legality refuses the control range, DEL, and the FAT illegal set" {
    const legal = fs.lfn.internal_unit_is_lfn_legal;
    try std.testing.expectEqual(@as(u8, 0), legal(0x00));
    try std.testing.expectEqual(@as(u8, 0), legal(0x1F));
    try std.testing.expectEqual(@as(u8, 0), legal(0x7F));
    for ([_]u16{ '"', '*', '/', ':', '<', '>', '?', '\\', '|' }) |ch| {
        try std.testing.expectEqual(@as(u8, 0), legal(ch));
    }
}

test "LFN legality allows the characters VFAT deliberately allows" {
    const legal = fs.lfn.internal_unit_is_lfn_legal;
    for ([_]u16{ ' ', '+', ',', ';', '=', '[', ']' }) |ch| {
        try std.testing.expectEqual(@as(u8, 1), legal(ch));
    }
}

test "LFN legality allows ordinary and non-ASCII units" {
    const legal = fs.lfn.internal_unit_is_lfn_legal;
    try std.testing.expectEqual(@as(u8, 1), legal('a'));
    try std.testing.expectEqual(@as(u8, 1), legal('A'));
    try std.testing.expectEqual(@as(u8, 1), legal('0'));
    try std.testing.expectEqual(@as(u8, 1), legal(0xE9));
}

test "83 legality refuses the short-name exclusions and the ASCII cut" {
    const legal = fs.lfn.internal_unit_is_83_legal;
    for ([_]u16{ '.', '+', ',', ';', '=', '[', ']', ' ', 0xE9 }) |ch| {
        try std.testing.expectEqual(@as(u8, 0), legal(ch));
    }
}

test "83 legality allows ordinary name characters and pins the DEL fall-through" {
    const legal = fs.lfn.internal_unit_is_83_legal;
    for ([_]u16{ 'a', 'A', '0', '9', '_', '-' }) |ch| {
        try std.testing.expectEqual(@as(u8, 1), legal(ch));
    }
    // DEL is excluded upstream by the LFN check (the documented precondition);
    // the port preserves the C fall-through so the two never disagree.
    try std.testing.expectEqual(@as(u8, 1), legal(0x7F));
}

test "priv_to_upper folds lower-case ASCII and leaves the rest" {
    const up = fs.lfn.priv_to_upper;
    try std.testing.expectEqual(@as(u8, 'A'), up('a'));
    try std.testing.expectEqual(@as(u8, 'Z'), up('z'));
    try std.testing.expectEqual(@as(u8, 'A'), up('A'));
    try std.testing.expectEqual(@as(u8, '0'), up('0'));
    try std.testing.expectEqual(@as(u8, 0xE9), up(0xE9));
}

test "name_is_83 refuses bad base and extension lengths" {
    const is83 = fs.lfn.internal_name_is_83;
    const dot_txt = [_]u16{ '.', 't', 'x', 't' };
    try std.testing.expectEqual(@as(u8, 0), is83(&dot_txt, 4)); // empty base
    const long_base = [_]u16{ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '.', 'T', 'X', 'T' };
    try std.testing.expectEqual(@as(u8, 0), is83(&long_base, 13)); // base 9
    const no_ext = [_]u16{ 'A', 'B', 'C', '.' };
    try std.testing.expectEqual(@as(u8, 0), is83(&no_ext, 4)); // trailing dot
    const long_ext = [_]u16{ 'A', 'B', 'C', '.', 'A', 'B', 'C', 'D' };
    try std.testing.expectEqual(@as(u8, 0), is83(&long_ext, 8)); // ext 4
}

test "name_is_83 accepts well-formed names and refuses illegal units" {
    const is83 = fs.lfn.internal_name_is_83;
    const abc = [_]u16{ 'A', 'B', 'C', '.', 'T', 'X', 'T' };
    try std.testing.expectEqual(@as(u8, 1), is83(&abc, 7));
    const img = [_]u16{ 'I', 'M', 'G', '_', '0', '1' };
    try std.testing.expectEqual(@as(u8, 1), is83(&img, 6)); // no dot
    const plus = [_]u16{ 'A', 'B', '+', 'C', '.', 'T', 'X', 'T' };
    try std.testing.expectEqual(@as(u8, 0), is83(&plus, 8)); // '+' needs a long name
}

test "case_kind maps all-lower and all-upper halves to NTRes flags" {
    const kind = fs.lfn.internal_name_case_kind;
    var ntres: u8 = 0xFF;
    const upper = [_]u16{ 'A', 'B', 'C', '.', 'T', 'X', 'T' };
    try std.testing.expectEqual(c.k_name_kind_short, kind(&upper, 7, &ntres));
    try std.testing.expectEqual(@as(u8, 0), ntres);
    var ntres2: u8 = 0xFF;
    const lower = [_]u16{ 'a', 'b', 'c', '.', 't', 'x', 't' };
    try std.testing.expectEqual(c.k_name_kind_short, kind(&lower, 7, &ntres2));
    try std.testing.expectEqual(@as(u8, @intCast(c.k_ntres_base_lower | c.k_ntres_ext_lower)), ntres2);
}

test "case_kind refuses a mixed-case half" {
    const kind = fs.lfn.internal_name_case_kind;
    var ntres: u8 = 0xFF;
    const mixed = [_]u16{ 'A', 'b', 'C', '.', 'T', 'X', 'T' };
    try std.testing.expectEqual(c.k_name_kind_long, kind(&mixed, 7, &ntres));
    try std.testing.expectEqual(@as(u8, 0), ntres);
}

test "case_kind treats digits and punctuation as caseless" {
    const kind = fs.lfn.internal_name_case_kind;
    var ntres: u8 = 0xFF;
    const img = [_]u16{ 'I', 'M', 'G', '_', '0', '1' };
    try std.testing.expectEqual(c.k_name_kind_short, kind(&img, 6, &ntres));
    try std.testing.expectEqual(@as(u8, 0), ntres);
}

test "pack_base upper-cases the base and advances the cursor" {
    var buf: [11]u8 = undefined;
    @memset(&buf, ' ');
    var p: [*c]const u8 = "abc.def";
    const pp: [*c][*c]const u8 = &p;
    try std.testing.expectEqual(@as(u8, 1), fs.lfn.internal_pack_base(pp, &buf));
    p = pp.*;
    try std.testing.expectEqual(@as(u8, 'A'), buf[0]);
    try std.testing.expectEqual(@as(u8, 'B'), buf[1]);
    try std.testing.expectEqual(@as(u8, 'C'), buf[2]);
    try std.testing.expectEqual(@as(u8, '.'), p[0]);
}

test "pack_base refuses overflow and an empty base" {
    var buf: [11]u8 = undefined;
    @memset(&buf, ' ');
    const long_name: [*c]const u8 = "ABCDEFGHIJ.TXT";
    const long_pp: [*c][*c]const u8 = @constCast(&long_name);
    try std.testing.expectEqual(@as(u8, 0), fs.lfn.internal_pack_base(long_pp, &buf));
    const empty: [*c]const u8 = ".txt";
    const empty_pp: [*c][*c]const u8 = @constCast(&empty);
    try std.testing.expectEqual(@as(u8, 0), fs.lfn.internal_pack_base(empty_pp, &buf));
}

test "pack_base refuses a byte above the ASCII cut" {
    var buf: [11]u8 = undefined;
    @memset(&buf, ' ');
    const utf8: [*c]const u8 = "ab\xC3\xA9.txt";
    const pp: [*c][*c]const u8 = @constCast(&utf8);
    try std.testing.expectEqual(@as(u8, 0), fs.lfn.internal_pack_base(pp, &buf));
    try std.testing.expectEqual(@as(u8, 'A'), buf[0]);
    try std.testing.expectEqual(@as(u8, 'B'), buf[1]);
}

test "pack_ext packs the extension and succeeds with no dot" {
    var buf: [11]u8 = undefined;
    @memset(&buf, ' ');
    const base_only: [*c]const u8 = "\x00";
    try std.testing.expectEqual(@as(u8, 1), fs.lfn.internal_pack_ext(base_only, &buf));
    try std.testing.expectEqual(@as(u8, 1), fs.lfn.internal_pack_ext("txT", &buf)); // not at a dot
    try std.testing.expectEqual(@as(u8, 1), fs.lfn.internal_pack_ext(".txT", &buf));
    try std.testing.expectEqual(@as(u8, 'T'), buf[8]);
    try std.testing.expectEqual(@as(u8, 'X'), buf[9]);
    try std.testing.expectEqual(@as(u8, 'T'), buf[10]);
}

test "pack_ext refuses an extension over three characters" {
    var buf: [11]u8 = undefined;
    @memset(&buf, ' ');
    try std.testing.expectEqual(@as(u8, 0), fs.lfn.internal_pack_ext(".abcd", &buf));
}

test "path_to_83 packs, upper-cases, and space-pads" {
    var buf: [11]u8 = undefined;
    @memset(&buf, 0);
    try std.testing.expectEqual(@as(u8, 1), fs.lfn.priv_path_to_83("file.txt", &buf));
    try std.testing.expectEqual(@as(u8, 'F'), buf[0]);
    try std.testing.expectEqual(@as(u8, 'I'), buf[1]);
    try std.testing.expectEqual(@as(u8, 'L'), buf[2]);
    try std.testing.expectEqual(@as(u8, 'E'), buf[3]);
    try std.testing.expectEqual(@as(u8, ' '), buf[4]);
    try std.testing.expectEqual(@as(u8, 'T'), buf[8]);
    try std.testing.expectEqual(@as(u8, 'X'), buf[9]);
    try std.testing.expectEqual(@as(u8, 'T'), buf[10]);
}

test "path_to_83 skips leading slashes and handles no extension" {
    var buf: [11]u8 = undefined;
    @memset(&buf, 0);
    try std.testing.expectEqual(@as(u8, 1), fs.lfn.priv_path_to_83("//a.b", &buf));
    try std.testing.expectEqual(@as(u8, 'A'), buf[0]);
    try std.testing.expectEqual(@as(u8, 'B'), buf[8]);
    var buf2: [11]u8 = undefined;
    @memset(&buf2, 0);
    try std.testing.expectEqual(@as(u8, 1), fs.lfn.priv_path_to_83("abc", &buf2));
    try std.testing.expectEqual(@as(u8, ' '), buf2[8]);
}

test "path_to_83 refuses bad bases" {
    var buf: [11]u8 = undefined;
    @memset(&buf, ' ');
    try std.testing.expectEqual(@as(u8, 0), fs.lfn.priv_path_to_83("ABCDEFGHI.TXT", &buf));
    try std.testing.expectEqual(@as(u8, 0), fs.lfn.priv_path_to_83(".txt", &buf));
}

test "path_to_83 refuses an over-long extension" {
    var buf: [11]u8 = undefined;
    @memset(&buf, ' ');
    try std.testing.expectEqual(@as(u8, 0), fs.lfn.priv_path_to_83("ab.abcd", &buf));
}

test "path_to_83 refuses null arguments" {
    var buf: [11]u8 = undefined;
    @memset(&buf, ' ');
    try std.testing.expectEqual(@as(u8, 0), fs.lfn.priv_path_to_83(null, &buf));
    try std.testing.expectEqual(@as(u8, 0), fs.lfn.priv_path_to_83("a.b", null));
}

test "classify: short name gets lower-case NTRes bits" {
    var units: [c.k_lfn_write_max]u16 = undefined;
    var n83: [11]u8 = undefined;
    var ntres: u8 = 0xFF;
    var nunits: u32 = 0;
    try std.testing.expectEqual(c.k_name_kind_short, fs.lfn.priv_name_classify("abc.txt", &units, &nunits, &n83, &ntres));
    try std.testing.expectEqual(@as(u32, 7), nunits);
    try std.testing.expectEqual(@as(u8, ' '), n83[3]);
    try std.testing.expectEqual(@as(u8, 'A'), n83[0]);
    try std.testing.expectEqual(@as(u8, 'T'), n83[8]);
    try std.testing.expectEqual(c.k_ntres_base_lower | c.k_ntres_ext_lower, ntres);
}

test "classify: all-upper name keeps NTRes at zero" {
    var units: [c.k_lfn_write_max]u16 = undefined;
    var n83: [11]u8 = undefined;
    var ntres: u8 = 0xFF;
    var nunits: u32 = 0;
    try std.testing.expectEqual(c.k_name_kind_short, fs.lfn.priv_name_classify("ABC.TXT", &units, &nunits, &n83, &ntres));
    try std.testing.expectEqual(@as(u32, 7), nunits);
    try std.testing.expectEqual(@as(u8, 0), ntres);
}

test "classify: mixed-case name needs a long chain" {
    var units: [c.k_lfn_write_max]u16 = undefined;
    var n83: [11]u8 = undefined;
    var ntres: u8 = 0xFF;
    var nunits: u32 = 0;
    try std.testing.expectEqual(c.k_name_kind_long, fs.lfn.priv_name_classify("MiXeD.txt", &units, &nunits, &n83, &ntres));
    try std.testing.expectEqual(@as(u8, 0), ntres);
}

test "classify: an LFN-illegal unit kills the name" {
    var units: [c.k_lfn_write_max]u16 = undefined;
    var n83: [11]u8 = undefined;
    var ntres: u8 = 0xFF;
    var nunits: u32 = 0;
    try std.testing.expectEqual(c.k_name_kind_invalid, fs.lfn.priv_name_classify("a\x01b", &units, &nunits, &n83, &ntres));
    try std.testing.expectEqual(@as(u32, 3), nunits);
}

test "classify: null arguments are invalid" {
    var units: [c.k_lfn_write_max]u16 = undefined;
    var n83: [11]u8 = undefined;
    var ntres: u8 = 0xFF;
    var nunits: u32 = 0;
    try std.testing.expectEqual(c.k_name_kind_invalid, fs.lfn.priv_name_classify(null, &units, &nunits, &n83, &ntres));
}
