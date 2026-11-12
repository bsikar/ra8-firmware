//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Untrusted-byte parsers for the tar walker: numerics, checksum, pax.
//! Ported from src/unarch_tar_fields.c (RA8FW-1197) behind the unchanged
//! declarations in src/unarch_tar_internal.h -- the priv_* C ABI the host
//! tests drive stays exactly as declared. Grammar sources: the POSIX.1-2017
//! pax interchange format and the documented GNU tar base-256 numeric
//! extension. Clean-room: no code from GNU tar / libarchive / busybox.

const std = @import("std");

const k_ra8_ok: u16 = 0;
const k_ra8_err_validation_failed: u16 = 0x501;
const k_ra8_err_null_ptr: u16 = 0x504;

/// tar_field_const_t: grammar constants for the numeric and pax parsers.
const k_tar_octal_base: u64 = 8;
const k_tar_decimal_base: u64 = 10;
const k_tar_b256_flag: u8 = 0x80;
const k_tar_b256_negative: u8 = 0x40;
const k_tar_b256_payload: u8 = 0x7F;
const k_tar_b256_fit: usize = 8;
const k_tar_pax_len_max: usize = 7;
const k_tar_chksum_space: u8 = ' ';

/// ra8_tar_layout_t: ustar header layout (unarch_tar_internal.h).
pub const k_ra8_tar_off_name: usize = 0;
pub const k_ra8_tar_len_name: usize = 100;
pub const k_ra8_tar_off_size: usize = 124;
pub const k_ra8_tar_len_size: usize = 12;
pub const k_ra8_tar_off_chksum: usize = 148;
pub const k_ra8_tar_len_chksum: usize = 8;
pub const k_ra8_tar_off_type: usize = 156;
pub const k_ra8_tar_off_magic: usize = 257;
pub const k_ra8_tar_len_magic: usize = 5;
pub const k_ra8_tar_off_magic_term: usize = 262;
pub const k_ra8_tar_off_prefix: usize = 345;
pub const k_ra8_tar_len_prefix: usize = 155;

/// ra8_tar_type_t: classified member kinds.
pub const k_ra8_tar_type_file: u8 = 0;
pub const k_ra8_tar_type_dir: u8 = 1;
pub const k_ra8_tar_type_pax: u8 = 2;
pub const k_ra8_tar_type_meta: u8 = 3;
pub const k_ra8_tar_type_longname: u8 = 4;
pub const k_ra8_tar_type_other: u8 = 5;

/// Decode an octal ASCII field (leading spaces, NUL/space terminated).
/// Rejects an empty digit run, a non-octal byte before the terminator, and
/// 64-bit overflow. On error `out` is untouched (the caller zeroed it).
fn internal_octal(field: []const u8, out: *u64) u16 {
    var i: usize = 0;
    while (i < field.len and field[i] == ' ') : (i += 1) {}
    var v: u64 = 0;
    var digits: usize = 0;
    while (i < field.len and field[i] != 0 and field[i] != ' ') : (i += 1) {
        const c = field[i];
        if (c < '0' or c > '7') {
            return k_ra8_err_validation_failed;
        }
        const d: u64 = c - '0';
        if (v > (std.math.maxInt(u64) - d) / k_tar_octal_base) {
            return k_ra8_err_validation_failed; // would overflow uint64
        }
        v = v * k_tar_octal_base + d;
        digits += 1;
    }
    if (digits == 0) {
        return k_ra8_err_validation_failed; // empty field
    }
    out.* = v;
    return k_ra8_ok;
}

/// Decode a GNU base-256 (big-endian binary) numeric field: byte 0 carries
/// the 0x80 marker. Negative values and values that cannot fit a u64 are
/// rejected fail-closed.
fn internal_base256(field: []const u8, out: *u64) u16 {
    if ((field[0] & k_tar_b256_negative) != 0) {
        return k_ra8_err_validation_failed; // negative size: hostile
    }
    if ((field[0] & k_tar_b256_payload) != 0) {
        return k_ra8_err_validation_failed; // > 2^(8*(len-1)): way over uint64
    }
    var v: u64 = 0;
    var i: usize = 1;
    while (i < field.len) : (i += 1) {
        const remaining = field.len - i;
        if (remaining > k_tar_b256_fit) {
            if (field[i] != 0) {
                return k_ra8_err_validation_failed; // over 64 bits
            }
            continue;
        }
        v = (v << 8) | field[i];
    }
    out.* = v;
    return k_ra8_ok;
}

/// priv_unarch_tar_num: octal, or GNU base-256 when the marker bit is set.
pub export fn priv_unarch_tar_num(field: ?[*]const u8, len: usize, out: ?*u64) callconv(.c) u16 {
    if (field == null or out == null) {
        return k_ra8_err_null_ptr;
    }
    out.?.* = 0;
    if (len == 0) {
        return k_ra8_err_validation_failed;
    }
    if ((field.?[0] & k_tar_b256_flag) != 0) {
        return internal_base256(field.?[0..len], out.?);
    }
    return internal_octal(field.?[0..len], out.?);
}

/// priv_unarch_tar_block_zero: whether a whole 512-byte block is zero.
pub export fn priv_unarch_tar_block_zero(block: ?[*]const u8) callconv(.c) bool {
    if (block == null) {
        return false;
    }
    for (0..512) |i| {
        if (block.?[i] != 0) {
            return false;
        }
    }
    return true;
}

/// priv_unarch_tar_checksum_ok: signed-byte sum with the chksum field read
/// as spaces, against the stored octal value.
pub export fn priv_unarch_tar_checksum_ok(block: ?[*]const u8) callconv(.c) bool {
    if (block == null) {
        return false;
    }
    var sum: u32 = 0;
    for (0..512) |i| {
        const in_chksum = i >= k_ra8_tar_off_chksum and i < k_ra8_tar_off_chksum + k_ra8_tar_len_chksum;
        sum += if (in_chksum) @as(u32, k_tar_chksum_space) else @as(u32, block.?[i]);
    }
    var stored: u64 = 0;
    if (internal_octal(block.?[k_ra8_tar_off_chksum..][0..k_ra8_tar_len_chksum], &stored) != k_ra8_ok) {
        return false;
    }
    return stored == @as(u64, sum);
}

/// priv_unarch_tar_magic_ok: "ustar" at 257, then NUL or ' ' at 262.
pub export fn priv_unarch_tar_magic_ok(block: ?[*]const u8) callconv(.c) bool {
    if (block == null) {
        return false;
    }
    const magic = "ustar";
    for (magic, 0..) |m, i| {
        if (block.?[k_ra8_tar_off_magic + i] != m) {
            return false;
        }
    }
    const term = block.?[k_ra8_tar_off_magic_term];
    return term == 0 or term == ' ';
}

/// priv_unarch_tar_classify: map a raw typeflag byte onto the walker's kinds.
pub export fn priv_unarch_tar_classify(typeflag: u8) callconv(.c) u8 {
    return switch (typeflag) {
        '0' => k_ra8_tar_type_file,
        0 => k_ra8_tar_type_file, // pre-POSIX regular-file flag
        '5' => k_ra8_tar_type_dir,
        'x' => k_ra8_tar_type_pax,
        'g' => k_ra8_tar_type_meta,
        'K' => k_ra8_tar_type_meta,
        'L' => k_ra8_tar_type_longname,
        else => k_ra8_tar_type_other,
    };
}

/// Decode one pax record's decimal length prefix: digits up to
/// k_tar_pax_len_max, terminated by one space; the length must cover at
/// least the prefix itself. Caller still range-checks reclen against len.
fn internal_pax_reclen(data: []const u8, reclen: *usize, body: *usize) u16 {
    var v: u64 = 0;
    var digits: usize = 0;
    var i: usize = 0;
    while (i < data.len) : (i += 1) {
        const c = data[i];
        if (c == ' ') {
            break;
        }
        if (c < '0' or c > '9') {
            return k_ra8_err_validation_failed;
        }
        if (digits >= k_tar_pax_len_max) {
            return k_ra8_err_validation_failed; // absurd length prefix
        }
        v = v * k_tar_decimal_base + (c - '0');
        digits += 1;
    }
    if (digits == 0) {
        return k_ra8_err_validation_failed; // no digits before the space
    }
    if (i >= data.len) {
        return k_ra8_err_validation_failed; // ran out before the space
    }
    reclen.* = @intCast(v);
    body.* = i + 1;
    return k_ra8_ok;
}

/// Decimal-decode a pax `size` record value: plain non-negative decimal
/// digits only; empty, non-digit or overflowing rejects fail-closed.
fn internal_pax_size_value(val: []const u8, out: *u64) u16 {
    if (val.len == 0) {
        return k_ra8_err_validation_failed;
    }
    var v: u64 = 0;
    for (val) |c| {
        if (c < '0' or c > '9') {
            return k_ra8_err_validation_failed;
        }
        const d: u64 = c - '0';
        if (v > (std.math.maxInt(u64) - d) / k_tar_decimal_base) {
            return k_ra8_err_validation_failed; // would overflow uint64
        }
        v = v * k_tar_decimal_base + d;
    }
    out.* = v;
    return k_ra8_ok;
}

/// Apply one parsed pax record: `path` copies into the caller name buffer
/// (clamped), `size` decimal-decodes, every other key is ignored.
fn internal_pax_apply(
    key: []const u8,
    val: []const u8,
    name_buf: ?[*]u8,
    name_cap: u16,
    name_len: *u16,
    have_path: *bool,
    size_ovr: *u64,
    have_size: *bool,
) u16 {
    if (std.mem.eql(u8, key, "path")) {
        const want = if (val.len > name_cap) @as(usize, name_cap) else val.len;
        if (want > 0) {
            @memcpy(name_buf.?[0..want], val[0..want]);
        }
        name_len.* = @intCast(want);
        have_path.* = true;
        return k_ra8_ok;
    }
    if (std.mem.eql(u8, key, "size")) {
        const verr = internal_pax_size_value(val, size_ovr);
        if (verr != k_ra8_ok) {
            return verr;
        }
        have_size.* = true;
    }
    return k_ra8_ok;
}

/// priv_unarch_tar_pax_parse: walk length-prefixed `%d key=value\n` records
/// with a fail-closed guard at every step. Outputs are caller-initialised
/// (same contract as the C TU: nothing is zeroed here).
pub export fn priv_unarch_tar_pax_parse(
    data: ?[*]const u8,
    len: usize,
    name_buf: ?[*]u8,
    name_cap: u16,
    name_len: ?*u16,
    have_path: ?*bool,
    size_ovr: ?*u64,
    have_size: ?*bool,
) callconv(.c) u16 {
    if (data == null or name_len == null or have_path == null or size_ovr == null or have_size == null) {
        return k_ra8_err_null_ptr;
    }
    if (name_cap > 0 and name_buf == null) {
        return k_ra8_err_null_ptr;
    }
    var off: usize = 0;
    while (off < len) {
        var reclen: usize = 0;
        var body: usize = 0;
        const lerr = internal_pax_reclen(data.?[off..len], &reclen, &body);
        if (lerr != k_ra8_ok) {
            return lerr;
        }
        if (reclen <= body) {
            return k_ra8_err_validation_failed; // length cannot cover its prefix
        }
        if (reclen > len - off) {
            return k_ra8_err_validation_failed; // record overruns the data
        }
        if (data.?[off + reclen - 1] != '\n') {
            return k_ra8_err_validation_failed; // record must end in newline
        }
        const kv = data.?[off + body ..][0 .. reclen - body - 1]; // strip the newline
        var key_len: usize = 0;
        while (key_len < kv.len and kv[key_len] != '=') : (key_len += 1) {}
        if (key_len == kv.len) {
            return k_ra8_err_validation_failed; // no key/value delimiter
        }
        const aerr = internal_pax_apply(
            kv[0..key_len],
            kv[key_len + 1 ..],
            name_buf,
            name_cap,
            name_len.?,
            have_path.?,
            size_ovr.?,
            have_size.?,
        );
        if (aerr != k_ra8_ok) {
            return aerr;
        }
        off += reclen;
    }
    return k_ra8_ok;
}
