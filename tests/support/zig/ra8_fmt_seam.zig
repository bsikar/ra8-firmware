//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host seam stand-ins for the C ABI acceptance test of `libs/ra8_fmt` so the
//! root build graph verifies `libra8_fmt.a` end-to-end without compiling the
//! full third-party codec dependencies into the root host slice.

const std = @import("std");

const ok: u16 = 0;
const err_not_supported: u16 = 0x107;

const JofInfo = extern struct {
    width: u16,
    height: u16,
    tile_w: u16,
    tile_h: u16,
    tile_cols: u16,
    tile_rows: u16,
    bpp: u8,
    codec: u8,
    tile_count: u32,
    index_off: u32,
    total_size: u32,
};

const JofAuditRecord = extern struct {
    offset: u32,
    length: u32,
    payload: u32,
    content_hash: u32,
    width: u16,
    height: u16,
    uniform: bool,
};

const JofAuditWorkspace = extern struct {
    records: ?[*]JofAuditRecord,
    record_cap: u32,
    tile: ?[*]u8,
    tile_cap: u32,
    scratch: ?[*]u8,
    scratch_cap: u32,
};

const JofAuditResult = extern struct {
    info: JofInfo,
    decoded_tiles: u32,
    coverage_errors: u32,
    geometry_errors: u32,
    duplicate_candidates: u32,
};

const JofProduceCfg = extern struct {
    pull: ?*const fn (?*anyopaque, ?[*]u8, usize, ?*usize) callconv(.c) u16,
    pull_ctx: ?*anyopaque,
    sink: ?*const fn (?*anyopaque, ?[*]const u8, usize) callconv(.c) u16,
    sink_ctx: ?*anyopaque,
    tile_w: u16,
    tile_h: u16,
    codec: u8,
    max_width: u16,
    max_height: u16,
    work: ?[*]u8,
    work_cap: usize,
    webp_work: ?[*]u8,
    webp_work_cap: usize,
};

const BookHeader = extern struct {
    magic: [8]u8,
    rest: [23]u32,
};

const BookChunked = extern struct {
    file_read: ?*const anyopaque,
    file_ctx: ?*anyopaque,
    inflate_cb: ?*const anyopaque,
    table: ?[*]const u64,
    table_cap_entries: u32,
    staging: ?[*]u8,
    staging_cap: u32,
    payload_off: u64,
    inflated_total: u64,
    chunk_bytes: u32,
    chunk_count: u32,
};

export fn jof_work_bytes(max_w: u16, max_h: u16, tile_w: u16, tile_h: u16) u32 {
    _ = tile_w;
    if (max_w == 0 or max_h == 0) return 0;
    return @as(u32, max_w) * @as(u32, max_h) + tile_h;
}

export fn jof_webp_work_bytes(max_w: u16, max_h: u16, src_len: u32) u32 {
    return @as(u32, max_w) * @as(u32, max_h) * 4 + src_len;
}

export fn jof_produce(cfg: *const JofProduceCfg, info: *JofInfo) u16 {
    const hdr = [_]u8{ @intCast(cfg.max_width & 0xFF), @intCast(cfg.max_height & 0xFF), @intCast(cfg.tile_h & 0xFF), 3 };
    const rc = cfg.sink.?(cfg.sink_ctx, &hdr, hdr.len);
    if (rc != ok) return rc;
    const rows = (@as(u16, cfg.max_height) + cfg.tile_h - 1) / cfg.tile_h;
    info.* = .{
        .width = cfg.max_width,
        .height = cfg.max_height,
        .tile_w = cfg.tile_w,
        .tile_h = cfg.tile_h,
        .tile_cols = 1,
        .tile_rows = rows,
        .bpp = 3,
        .codec = cfg.codec,
        .tile_count = rows,
        .index_off = 32,
        .total_size = 4,
    };
    return ok;
}

export fn jof_stored_bound(raw: u32) u32 {
    return raw + raw / 8 + 256;
}

export fn jof_parse(pread: ?*const anyopaque, ctx: ?*anyopaque, sz: u64, info: *JofInfo) u16 {
    _ = sz;
    var header: [4]u8 = undefined;
    var got: usize = 0;
    const pread_fn: *const fn (?*anyopaque, u64, ?[*]u8, usize, ?*usize) callconv(.c) u16 = @ptrCast(@alignCast(pread.?));
    const rc = pread_fn(ctx, 0, &header, 4, &got);
    if (rc != ok) return rc;
    info.tile_h = header[2];
    const rows = (@as(u16, info.height) + info.tile_h - 1) / info.tile_h;
    info.tile_rows = rows;
    info.tile_count = rows;
    return ok;
}

export fn jof_read_tile(
    pread: ?*const anyopaque,
    ctx: ?*anyopaque,
    info: *const JofInfo,
    tx: u16,
    ty: u16,
    scratch: ?[*]u8,
    sc: u32,
    px: ?[*]u8,
    oc: u32,
    ow: *u16,
    oh: *u16,
) u16 {
    _ = pread;
    _ = ctx;
    _ = tx;
    _ = scratch;
    _ = sc;
    _ = oc;
    const th: u16 = @intCast(@min(@as(u32, info.tile_h), info.height - @as(u32, ty) * info.tile_h));
    const row_bytes = @as(usize, info.width) * info.bpp;
    for (0..th) |r| {
        const val: u8 = @intCast((@as(usize, ty) * info.tile_h + r + 11) & 0xFF);
        @memset(px.?[r * row_bytes ..][0..row_bytes], val);
    }
    ow.* = info.width;
    oh.* = th;
    return ok;
}

export fn jof_audit(
    pread: ?*const anyopaque,
    ctx: ?*anyopaque,
    sz: u64,
    ws: *JofAuditWorkspace,
    out: *JofAuditResult,
) u16 {
    _ = pread;
    _ = ctx;
    _ = sz;
    out.* = std.mem.zeroes(JofAuditResult);
    out.info = .{
        .width = 4,
        .height = 2,
        .tile_w = 4,
        .tile_h = 1,
        .tile_cols = 1,
        .tile_rows = 2,
        .bpp = 1,
        .codec = 0,
        .tile_count = 2,
        .index_off = 36,
        .total_size = 68,
    };
    if (ws.records != null and ws.record_cap >= 2) {
        ws.records.?[0] = .{ .offset = 32, .length = 2, .payload = 2, .content_hash = 1, .width = 4, .height = 1, .uniform = false };
        ws.records.?[1] = .{ .offset = 34, .length = 2, .payload = 2, .content_hash = 2, .width = 4, .height = 1, .uniform = false };
    }
    return ok;
}

export fn book_chunked_open(
    rd: *BookChunked,
    fr: ?*const anyopaque,
    fc: ?*anyopaque,
    fl: u64,
    inf: ?*const anyopaque,
    tbl: ?[*]u64,
    tc: u32,
    st: ?[*]u8,
    sc: u32,
) u16 {
    _ = fl;
    if (tc < 2) return 0x105;
    tbl.?[0] = 0;
    tbl.?[1] = 16;
    rd.* = .{
        .file_read = fr,
        .file_ctx = fc,
        .inflate_cb = inf,
        .table = tbl,
        .table_cap_entries = tc,
        .staging = st,
        .staging_cap = sc,
        .payload_off = 32,
        .inflated_total = 128,
        .chunk_bytes = 128,
        .chunk_count = 1,
    };
    return ok;
}

export fn book_chunked_validate_strict(
    rd: *BookChunked,
    c: ?[*]u8,
    cc: u32,
    s: ?[*]u8,
    sc: u32,
    hdr: *BookHeader,
) u16 {
    _ = rd;
    _ = c;
    _ = cc;
    _ = s;
    _ = sc;
    hdr.* = std.mem.zeroes(BookHeader);
    return ok;
}

export fn ra8_imgdec_sniff(bytes: [*]const u8, count: u32, out: *u32) u16 {
    if (count >= 8 and std.mem.eql(u8, bytes[0..8], "\x89PNG\r\n\x1a\n")) {
        out.* = 2;
        return ok;
    }
    out.* = 0;
    return err_not_supported;
}

export fn ra8_webp_get_info(data: [*]const u8, size: usize, ow: *u32, oh: *u32) u16 {
    _ = data;
    _ = size;
    ow.* = 0;
    oh.* = 0;
    return err_not_supported;
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
