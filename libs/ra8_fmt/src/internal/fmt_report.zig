//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Bounded report formatting helpers writing directly to `ra8_fmt_sink_t`.
//! Uses fixed stack buffers and emits no trailing NUL byte or locale dependency.

const c_abi = @import("c_abi");

const max_dec_digits: usize = 20;
const max_hex_digits: usize = 16;
const dec_radix: u64 = 10;
const hex_digits = "0123456789ABCDEF";
const empty_byte: [1]u8 = .{0};

/// Appends `bytes` to `sink`, preserving any caller error code.
pub fn put(sink: *const c_abi.Sink, bytes: []const u8) c_abi.Ra8Err {
    const write_fn = sink.write orelse return c_abi.k_ra8_err_null_ptr;
    const ptr: [*]const u8 = if (bytes.len > 0) bytes.ptr else &empty_byte;
    return write_fn(sink.ctx, ptr, bytes.len);
}

/// Appends a single byte character to `sink`.
pub fn putChar(sink: *const c_abi.Sink, ch: u8) c_abi.Ra8Err {
    const buf: [1]u8 = .{ch};
    return put(sink, &buf);
}

/// Appends a zero-terminated C string up to (excluding) the first NUL byte.
pub fn putCStr(sink: *const c_abi.Sink, cstr: [*:0]const u8) c_abi.Ra8Err {
    var len: usize = 0;
    while (cstr[len] != 0) : (len += 1) {}
    return put(sink, cstr[0..len]);
}

/// Renders `value` in unsigned base-10, left-padded with spaces to `width`.
pub fn putU64Padded(sink: *const c_abi.Sink, value: u64, width: u32) c_abi.Ra8Err {
    var rev: [max_dec_digits]u8 = undefined;
    var count: usize = 0;
    var v = value;
    while (true) {
        rev[count] = '0' + @as(u8, @intCast(v % dec_radix));
        count += 1;
        v /= dec_radix;
        if (v == 0) break;
    }

    var pad = width;
    while (pad > count) : (pad -= 1) {
        const rc = putChar(sink, ' ');
        if (rc != c_abi.k_ra8_ok) return rc;
    }

    var fwd: [max_dec_digits]u8 = undefined;
    for (0..count) |i| {
        fwd[i] = rev[count - 1 - i];
    }
    return put(sink, fwd[0..count]);
}

/// Appends `value` in uppercase hexadecimal of fixed digit `width` (<= 16).
pub fn putHex(sink: *const c_abi.Sink, value: u64, width: u32) c_abi.Ra8Err {
    if (width > max_hex_digits) return c_abi.k_ra8_err_invalid_size;
    var buf: [max_hex_digits]u8 = undefined;
    for (0..width) |i| {
        const shift: u6 = @intCast((width - i - 1) * 4);
        buf[i] = hex_digits[@as(usize, @intCast((value >> shift) & 0xF))];
    }
    return put(sink, buf[0..width]);
}

/// Emits `"<label><value><suffix>"`, halting on the first failing write.
pub fn putValueLine(
    sink: *const c_abi.Sink,
    label: []const u8,
    value: u64,
    suffix: []const u8,
) c_abi.Ra8Err {
    var rc = put(sink, label);
    if (rc == c_abi.k_ra8_ok) rc = putU64Padded(sink, value, 0);
    if (rc == c_abi.k_ra8_ok) rc = put(sink, suffix);
    return rc;
}

/// Appends `<value><suffix>` while `status.* == k_ra8_ok`.
pub fn appendField(
    sink: *const c_abi.Sink,
    value: u64,
    suffix: []const u8,
    status: *c_abi.Ra8Err,
) void {
    if (status.* == c_abi.k_ra8_ok) {
        status.* = putU64Padded(sink, value, 0);
    }
    if (status.* == c_abi.k_ra8_ok) {
        status.* = put(sink, suffix);
    }
}
