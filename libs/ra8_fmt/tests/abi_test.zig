//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI contract tests exercising `src/ra8_fmt_abi.zig` directly through the
//! six exported `ra8_fmt_*` symbols.

const std = @import("std");
const c_abi = @import("c_abi");
const abi = @import("ra8_fmt_abi");

export fn jof_work_bytes(max_w: u16, max_h: u16, tile_w: u16, tile_h: u16) u32 {
    _ = tile_w;
    if (max_w == 0 or max_h == 0) return 0;
    return @as(u32, max_w) * @as(u32, max_h) + @as(u32, tile_h);
}

export fn jof_webp_work_bytes(max_w: u16, max_h: u16, src_len: u32) u32 {
    return @as(u32, max_w) * @as(u32, max_h) * 4 + src_len;
}

export fn jof_produce(cfg: *const c_abi.JofProduceCfg, info: *c_abi.JofInfo) c_abi.Ra8Err {
    _ = cfg;
    info.* = std.mem.zeroes(c_abi.JofInfo);
    return c_abi.k_ra8_ok;
}

export fn jof_stored_bound(raw: u32) u32 {
    return raw + raw / 8 + 256;
}

export fn jof_parse(pread: ?c_abi.PreadFn, ctx: ?*anyopaque, sz: u64, info: *c_abi.JofInfo) c_abi.Ra8Err {
    _ = pread;
    _ = ctx;
    _ = sz;
    _ = info;
    return c_abi.k_ra8_ok;
}

export fn jof_read_tile(
    pread: ?c_abi.PreadFn,
    ctx: ?*anyopaque,
    info: *const c_abi.JofInfo,
    tx: u16,
    ty: u16,
    scratch: ?[*]u8,
    sc: u32,
    px: ?[*]u8,
    oc: u32,
    ow: *u16,
    oh: *u16,
) c_abi.Ra8Err {
    _ = pread;
    _ = ctx;
    _ = info;
    _ = tx;
    _ = ty;
    _ = scratch;
    _ = sc;
    _ = px;
    _ = oc;
    ow.* = 0;
    oh.* = 0;
    return c_abi.k_ra8_ok;
}

export fn jof_audit(
    pread: ?c_abi.PreadFn,
    ctx: ?*anyopaque,
    sz: u64,
    ws: *c_abi.JofAuditWorkspace,
    out: *c_abi.JofAuditResult,
) c_abi.Ra8Err {
    _ = pread;
    _ = ctx;
    _ = sz;
    _ = ws;
    out.* = std.mem.zeroes(c_abi.JofAuditResult);
    return c_abi.k_ra8_ok;
}

export fn book_chunked_open(
    rd: *c_abi.BookChunked,
    fr: ?c_abi.VsourceReadFn,
    fc: ?*anyopaque,
    fl: u64,
    inf: ?c_abi.BookInflateFn,
    tbl: ?[*]u64,
    tc: u32,
    st: ?[*]u8,
    sc: u32,
) c_abi.Ra8Err {
    _ = fr;
    _ = fc;
    _ = fl;
    _ = inf;
    _ = tbl;
    _ = tc;
    _ = st;
    _ = sc;
    rd.* = std.mem.zeroes(c_abi.BookChunked);
    return c_abi.k_ra8_ok;
}

export fn book_chunked_validate_strict(
    rd: *c_abi.BookChunked,
    c: ?[*]u8,
    cc: u32,
    s: ?[*]u8,
    sc: u32,
    hdr: *c_abi.BookHeader,
) c_abi.Ra8Err {
    _ = rd;
    _ = c;
    _ = cc;
    _ = s;
    _ = sc;
    hdr.* = std.mem.zeroes(c_abi.BookHeader);
    return c_abi.k_ra8_ok;
}

export fn ra8_imgdec_sniff(bytes: [*]const u8, count: u32, out: *u32) c_abi.Ra8Err {
    if (count >= 8 and std.mem.eql(u8, bytes[0..8], "\x89PNG\r\n\x1a\n")) {
        out.* = c_abi.k_imgdec_format_png;
        return c_abi.k_ra8_ok;
    }
    out.* = c_abi.k_imgdec_format_none;
    return c_abi.k_ra8_err_not_supported;
}

export fn ra8_webp_get_info(data: [*]const u8, size: usize, ow: *u32, oh: *u32) c_abi.Ra8Err {
    _ = data;
    _ = size;
    ow.* = 0;
    oh.* = 0;
    return c_abi.k_ra8_err_not_supported;
}

export fn tinfl_decompress_mem_to_mem(
    out_buf: ?*anyopaque,
    out_len: usize,
    src: ?*const anyopaque,
    src_len: usize,
    flags: c_int,
) usize {
    _ = out_buf;
    _ = out_len;
    _ = src;
    _ = src_len;
    _ = flags;
    return 0;
}

const png_fixture = [_]u8{
    0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
    0,    0,   0,   13,  'I',  'H',  'D',  'R',
    0,    0,   0,   12,  0,    0,    0,    6,
    8,    2,   0,   0,   0,    0,    0,    0,
};

fn readFixture(ctx: ?*anyopaque, offset: u64, buf: ?[*]u8, len: usize, got: ?*usize) callconv(.c) c_abi.Ra8Err {
    _ = ctx;
    if (offset >= png_fixture.len) {
        got.?.* = 0;
        return c_abi.k_ra8_ok;
    }
    const take = @min(len, png_fixture.len - @as(usize, @intCast(offset)));
    if (take > 0) @memcpy(buf.?[0..take], png_fixture[@intCast(offset)..][0..take]);
    got.?.* = take;
    return c_abi.k_ra8_ok;
}

test "C ABI exports reject null inputs across all six entry points" {
    try std.testing.expectEqual(c_abi.k_ra8_err_null_ptr, abi.ra8_fmt_jof_inspect_stream(null, false, null, null));
    try std.testing.expectEqual(c_abi.k_ra8_err_null_ptr, abi.ra8_fmt_rabook_inspect_stream(null, false, null, null));
    try std.testing.expectEqual(c_abi.k_ra8_err_null_ptr, abi.ra8_fmt_jof_convert_requirements(null, null));
    try std.testing.expectEqual(c_abi.k_ra8_err_null_ptr, abi.ra8_fmt_jof_convert_stream(null, null, null, null, null, null));
    try std.testing.expectEqual(c_abi.k_ra8_err_null_ptr, abi.ra8_fmt_jof_verify_requirements(null, null));
    try std.testing.expectEqual(c_abi.k_ra8_err_null_ptr, abi.ra8_fmt_jof_verify_stream(null, null, null, null, null, null, null, null, null));
}

test "C ABI convert and verify planners succeed end to end for PNG input" {
    const src = c_abi.Source{
        .read_at = readFixture,
        .validate = null,
        .ctx = null,
        .size = png_fixture.len,
    };
    var creq: c_abi.JofConvertRequirements = undefined;
    try std.testing.expectEqual(c_abi.k_ra8_ok, abi.ra8_fmt_jof_convert_requirements(&src, &creq));
    try std.testing.expectEqual(@as(u16, 12), creq.width);
    try std.testing.expectEqual(@as(u16, 6), creq.height);

    var vreq: c_abi.JofVerifyRequirements = undefined;
    try std.testing.expectEqual(c_abi.k_ra8_ok, abi.ra8_fmt_jof_verify_requirements(&src, &vreq));
    try std.testing.expectEqual(@as(u8, 3), vreq.bpp);
    try std.testing.expectEqual(@as(u32, 36), vreq.row_bytes);
}
