//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The name codec every FAT and exFAT path goes through: strict UTF-8 to
//! UTF-16LE and back, plus the two name predicates (case-folded compare and
//! all-ASCII). Both on-disk name formats store UTF-16, so every name a caller
//! hands in as UTF-8 crosses here.
//!
//! Decoding rejects what UTF-8 forbids: a stray continuation byte, a short
//! sequence, an overlong form, a surrogate spelled in three bytes (CESU-8),
//! and anything past U+10FFFF. Encoding rejects unpaired surrogates. The case
//! fold stays in C (`priv_exfat_upcase_unit`, the exFAT spec's table).
//!
//! Bounded loops: the decoder appends at least one unit per pass and stops at
//! `cap`; the encoder consumes at least one unit per pass and stops at `units`.

const c = @import("fs_c.zig").c;

pub const ok: u16 = c.k_ra8_ok;
pub const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
pub const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
pub const err_no_mem: u16 = c.k_ra8_err_no_mem;

const max_per_unit: u32 = 3;
const sur_hi: u32 = 0xD800;
const sur_lo: u32 = 0xDC00;
const sur_last: u32 = 0xDFFF;
const plane1: u32 = 0x10000;
const code_max: u32 = 0x10FFFF;

comptime {
    // Every UTF-8 name buffer is a UTF-16 unit cap times three, plus a NUL.
    const lfn_cap: u32 = @intCast(c.k_lfn_utf8_cap);
    const lfn_max: u32 = @intCast(c.k_lfn_write_max);
    const ex_cap: u32 = @intCast(c.k_exfat_name_u8_cap);
    const ex_max: u32 = @intCast(c.k_exfat_name_cap);
    if (lfn_cap != max_per_unit * lfn_max + 1) @compileError("k_lfn_utf8_cap must hold the longest VFAT long name in UTF-8");
    if (ex_cap != max_per_unit * ex_max + 1) @compileError("k_exfat_name_u8_cap must hold the longest exFAT name in UTF-8");
}

const Lead = struct { len: u32, cp: u32 };

/// Sequence length and payload bits of a lead byte; null for a stray
/// continuation byte or an 0xF8..0xFF byte.
fn lead(b: u8) ?Lead {
    const u: u32 = b;
    if (u <= 0x7F) return .{ .len = 1, .cp = u };
    if (u & 0xE0 == 0xC0) return .{ .len = 2, .cp = u & 0x1F };
    if (u & 0xF0 == 0xE0) return .{ .len = 3, .cp = u & 0x0F };
    if (u & 0xF8 == 0xF0) return .{ .len = 4, .cp = u & 0x07 };
    return null;
}

/// Rejects overlong forms, three-byte surrogates and code points past U+10FFFF.
fn wellFormed(cp: u32, len: u32) bool {
    return switch (len) {
        2 => cp >= 0x80,
        3 => cp >= 0x800 and (cp < sur_hi or cp > sur_last),
        4 => cp >= plane1 and cp <= code_max,
        else => true,
    };
}

/// Decodes the code point at `pos.*` and advances past it.
fn utf8Next(in: [*:0]const u8, pos: *u32) ?u32 {
    const p = pos.*;
    const l = lead(in[p]) orelse return null;
    var cp = l.cp;
    var k: u32 = 1;
    // A NUL is not a continuation byte, so this never reads past the string.
    while (k < l.len) : (k += 1) {
        const b: u32 = in[p + k];
        if (b & 0xC0 != 0x80) return null;
        cp = (cp << 6) | (b & 0x3F);
    }
    if (!wellFormed(cp, l.len)) return null;
    pos.* = p + l.len;
    return cp;
}

/// Appends `cp` as one unit or a surrogate pair.
fn utf16Put(cp: u32, out: [*]u16, cap: u32, n: *u32) u16 {
    const need: u32 = if (cp >= plane1) 2 else 1;
    if (n.* + need > cap) return err_no_mem;
    if (need == 1) {
        out[n.*] = @intCast(cp);
    } else {
        const rest = cp - plane1;
        out[n.*] = @intCast(sur_hi + (rest >> 10));
        out[n.* + 1] = @intCast(sur_lo + (rest & 0x3FF));
    }
    n.* += need;
    return ok;
}

/// UTF-8 (NUL-terminated) to UTF-16LE; `out_units` gets the unit count.
pub export fn priv_utf8_to_utf16(in: ?[*:0]const u8, out: ?[*]u16, cap: u32, out_units: ?*u32) callconv(.C) u16 {
    const s = in orelse return err_null_ptr;
    const o = out orelse return err_null_ptr;
    const units = out_units orelse return err_null_ptr;
    units.* = 0;
    var pos: u32 = 0;
    var n: u32 = 0;
    while (n <= cap) {
        if (s[pos] == 0) {
            units.* = n;
            return ok;
        }
        const cp = utf8Next(s, &pos) orelse return err_invalid_arg;
        const err = utf16Put(cp, o, cap, &n);
        if (err != ok) return err;
    }
    return err_no_mem; // unreachable: utf16Put refuses before n passes cap
}

/// Reads one code point (a BMP unit or a surrogate pair) at `i.*`.
fn utf16Take(in: [*]const u16, units: u32, i: *u32) ?u32 {
    const hi: u32 = in[i.*];
    if (hi < sur_hi or hi > sur_last) {
        i.* += 1;
        return hi;
    }
    if (hi >= sur_lo) return null; // a low surrogate with no high one before it
    if (i.* + 1 >= units) return null; // a high surrogate at the end
    const lo: u32 = in[i.* + 1];
    if (lo < sur_lo or lo > sur_last) return null;
    i.* += 2;
    return plane1 + (((hi - sur_hi) << 10) | (lo - sur_lo));
}

fn utf8Len(cp: u32) u32 {
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < plane1) return 3;
    return 4;
}

/// Appends `cp` in UTF-8, keeping room for the terminating NUL.
fn utf8Put(cp: u32, out: [*]u8, cap: u32, n: *u32) u16 {
    const len = utf8Len(cp);
    if (n.* + len + 1 > cap) return err_no_mem;
    const tags = [_]u32{ 0, 0, 0xC0, 0xE0, 0xF0 };
    var rest = cp;
    var k: u32 = len - 1;
    while (k >= 1) : (k -= 1) {
        out[n.* + k] = @intCast(0x80 | (rest & 0x3F));
        rest >>= 6;
    }
    out[n.*] = @intCast(tags[len] | rest);
    n.* += len;
    return ok;
}

/// UTF-16LE to NUL-terminated UTF-8. On overflow `out` is left empty.
pub export fn priv_utf16_to_utf8(in: ?[*]const u16, units: u32, out: ?[*]u8, cap: u32) callconv(.C) u16 {
    const s = in orelse return err_null_ptr;
    const o = out orelse return err_null_ptr;
    if (cap == 0) return err_null_ptr;
    o[0] = 0;
    var i: u32 = 0;
    var n: u32 = 0;
    while (i < units) {
        const cp = utf16Take(s, units, &i) orelse return err_invalid_arg;
        if (utf8Put(cp, o, cap, &n) != ok) {
            o[0] = 0;
            return err_no_mem;
        }
    }
    o[n] = 0;
    return ok;
}

/// 1 when the names match under the exFAT up-case fold.
pub export fn priv_utf16_ieq(a: [*]const u16, an: u32, b: [*]const u16, bn: u32) callconv(.C) u8 {
    if (an != bn) return 0;
    for (0..an) |i| {
        if (c.priv_exfat_upcase_unit(a[i]) != c.priv_exfat_upcase_unit(b[i])) return 0;
    }
    return 1;
}

/// 1 when every unit is 7-bit ASCII.
pub export fn priv_utf16_all_ascii(in: [*]const u16, units: u32) callconv(.C) u8 {
    for (in[0..units]) |u| {
        if (u > 0x7F) return 0;
    }
    return 1;
}
