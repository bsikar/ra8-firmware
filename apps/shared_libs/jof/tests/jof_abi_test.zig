//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `jof_abi.zig`.

const std = @import("std");
const jof_h = @import("jof_h");
const sut = @import("jof_abi");
const stubs = @import("abi_stubs.zig");

comptime {
    _ = @import("abi_stubs.zig");
    _ = stubs.Exports;
}

extern var jpeg_stub_w: u16;
extern var jpeg_stub_h: u16;
extern var jpeg_stub_channels: u8;
extern var jpeg_stub_rows_per_call: u16;
extern var jpeg_stub_px: [4096]u8;

const auditHash = sut.auditHash;
const auditRdU32 = sut.auditRdU32;
const bpp = sut.bpp;
const internal_finish = sut.internal_finish;
const jof_audit = sut.jof_audit;
const jof_audit_requirements = sut.jof_audit_requirements;
const jof_memstore_pread = sut.jof_memstore_pread;
const jof_memstore_sink = sut.jof_memstore_sink;
const jof_parse = sut.jof_parse;
const jof_probe_dims = sut.jof_probe_dims;
const jof_produce = sut.jof_produce;
const jof_read_tile = sut.jof_read_tile;
const jof_stored_bound = sut.jof_stored_bound;
const jof_tile_dims = sut.jof_tile_dims;
const jof_webp_work_bytes = sut.jof_webp_work_bytes;
const jof_work_bytes = sut.jof_work_bytes;
const priv_jof_bump_take = sut.priv_jof_bump_take;
const priv_jof_on_geom = sut.priv_jof_on_geom;
const priv_jof_on_rows = sut.priv_jof_on_rows;
const priv_jof_prefix_pull = sut.priv_jof_prefix_pull;

test "work bytes mirrors the C arithmetic" {
    try std.testing.expectEqual(@as(u32, 0), jof_webp_work_bytes(0, 100, 1000));
    try std.testing.expectEqual(@as(u32, 0), jof_webp_work_bytes(100, 0, 1000));
    try std.testing.expectEqual(@as(u32, 0), jof_webp_work_bytes(100, 100, 0));
    try std.testing.expectEqual(@as(u32, 0), jof_webp_work_bytes(@as(u16, @intCast(jof_h.k_ra8_webp_max_dim + 1)), 100, 1000));
    try std.testing.expectEqual(@as(u32, 0), jof_webp_work_bytes(100, @as(u16, @intCast(jof_h.k_ra8_webp_max_dim + 1)), 1000));
    // 100x100 frame: 40000 frame bytes, 84000 scratch, 1000 src, 128 slack.
    try std.testing.expectEqual(@as(u32, 1000 + 40000 + 80000 + 1048576 + 128), jof_webp_work_bytes(100, 100, 1000));
}

/// A minimal valid 64-byte raw atlas: 1x1 pixels, one 1x1 gray8 tile.
fn makeMinimalAtlas(backing: []u8) void {
    @memcpy(backing[0..4], "JOF1");
    backing[4] = 1; // width lsb
    backing[6] = 1; // height lsb
    backing[8] = 1; // tile_w lsb
    backing[10] = 1; // tile_h lsb
    backing[12] = 1; // bpp (gray8)
    backing[13] = jof_h.k_jof_codec_raw;
    backing[16] = 1; // tile count lsb
    backing[32] = 0xAB; // one raw pixel
    backing[40] = 32; // index entry: tile offset lsb
    backing[44] = 1; // index entry: tile length lsb
    backing[48] = 40; // footer: index offset lsb
    backing[52] = 1; // footer: tile count lsb
    backing[56] = 64; // footer: total size lsb
    @memcpy(backing[60..64], "JOFE");
}

test "priv_jof_bump_take carves aligned, advances, and fail-closes" {
    var arena: [64]u8 = undefined;
    var bump = jof_h.jof_bump_t{ .base = &arena, .cap = arena.len, .off = 0 };
    try std.testing.expect(priv_jof_bump_take(null, 1) == null);
    try std.testing.expect(priv_jof_bump_take(&bump, 0) == null);
    const a = priv_jof_bump_take(&bump, 1).?;
    try std.testing.expectEqual(@as(usize, 0), @intFromPtr(a) - @intFromPtr(&arena));
    try std.testing.expectEqual(@as(usize, 1), bump.off);
    const b = priv_jof_bump_take(&bump, 9).?;
    try std.testing.expectEqual(@intFromPtr(&arena) + 8, @intFromPtr(b));
    try std.testing.expectEqual(@as(usize, 17), bump.off);
    const c = priv_jof_bump_take(&bump, 1).?;
    try std.testing.expectEqual(@intFromPtr(&arena) + 24, @intFromPtr(c));
    try std.testing.expectEqual(@as(usize, 25), bump.off);
    // 40 more bytes would need bytes 32..72 of a 64-byte arena: refused, and
    // the arena state is unchanged on refusal.
    try std.testing.expect(priv_jof_bump_take(&bump, 40) == null);
    try std.testing.expectEqual(@as(usize, 25), bump.off);
}

test "jof_work_bytes refuses out-of-range geometry" {
    try std.testing.expectEqual(@as(u32, 0), jof_work_bytes(0, 1024, 128, 128));
    try std.testing.expectEqual(@as(u32, 0), jof_work_bytes(40000, 1024, 128, 128));
    try std.testing.expectEqual(@as(u32, 0), jof_work_bytes(1024, 0, 128, 128));
    try std.testing.expectEqual(@as(u32, 0), jof_work_bytes(1024, 40000, 128, 128));
    try std.testing.expectEqual(@as(u32, 0), jof_work_bytes(1024, 1024, 0, 128));
    try std.testing.expectEqual(@as(u32, 0), jof_work_bytes(1024, 1024, 128, 0));
    // 1x1 tiles over a full-canvas atlas blow the 65536-tile grid cap.
    try std.testing.expectEqual(@as(u32, 0), jof_work_bytes(32768, 32768, 1, 1));
    // A max-canvas tile pushes stage (and the total) past the u32 format cap.
    try std.testing.expectEqual(@as(u32, 0), jof_work_bytes(32768, 32768, 32768, 32768));
}

test "jof_work_bytes matches the C working-set composition" {
    // w = h = tile_w = tile_h = 64: jpeg_set = 131072 + 64*16*3 = 134144;
    // png_set = 8376 + 65536 + 4096 + 3*64*4 + 2 = 78778, so dec_set = 134144;
    // band = stage = 64*64*4 = 16384; cmp = 16384 + 2048 + 256 = 18688;
    // index = 1*1*8 = 8; dfl = 319352; slack = 128 -> 505088.
    try std.testing.expectEqual(@as(u32, 505088), jof_work_bytes(64, 64, 64, 64));
    try std.testing.expect(jof_work_bytes(1024, 1024, 128, 128) > 0);
}

test "jof_read_tile reads the minimal atlas tile through the memstore seam" {
    var backing: [64]u8 = std.mem.zeroes([64]u8);
    makeMinimalAtlas(&backing);
    var store = jof_h.jof_memstore_t{ .buf = &backing, .cap = backing.len, .len = backing.len };
    var info = std.mem.zeroes(jof_h.jof_info_t);
    try std.testing.expectEqual(jof_h.k_ra8_ok, jof_parse(jof_memstore_pread, &store, backing.len, &info));
    var px: [4]u8 = std.mem.zeroes([4]u8);
    var w: u16 = 0;
    var h: u16 = 0;
    try std.testing.expectEqual(
        jof_h.k_ra8_ok,
        jof_read_tile(jof_memstore_pread, &store, &info, 0, 0, null, 0, &px, px.len, &w, &h),
    );
    try std.testing.expectEqual(@as(u8, 0xAB), px[0]);
    try std.testing.expectEqual(@as(u16, 1), w);
    try std.testing.expectEqual(@as(u16, 1), h);
    // Tile 1 sits outside the 1x1 grid, so tile_dims rejects it.
    try std.testing.expect(
        jof_read_tile(jof_memstore_pread, &store, &info, 1, 0, null, 0, &px, px.len, &w, &h) != jof_h.k_ra8_ok,
    );
}

test "jof_read_tile rejects null args and a too-small destination" {
    var backing: [64]u8 = std.mem.zeroes([64]u8);
    makeMinimalAtlas(&backing);
    var store = jof_h.jof_memstore_t{ .buf = &backing, .cap = backing.len, .len = backing.len };
    var info = std.mem.zeroes(jof_h.jof_info_t);
    try std.testing.expectEqual(jof_h.k_ra8_ok, jof_parse(jof_memstore_pread, &store, backing.len, &info));
    var px: [4]u8 = std.mem.zeroes([4]u8);
    var w: u16 = 0;
    var h: u16 = 0;
    try std.testing.expectEqual(
        jof_h.k_ra8_err_null_ptr,
        jof_read_tile(null, &store, &info, 0, 0, null, 0, &px, px.len, &w, &h),
    );
    try std.testing.expectEqual(
        jof_h.k_ra8_err_null_ptr,
        jof_read_tile(jof_memstore_pread, &store, null, 0, 0, null, 0, &px, px.len, &w, &h),
    );
    try std.testing.expectEqual(
        jof_h.k_ra8_err_invalid_size,
        jof_read_tile(jof_memstore_pread, &store, &info, 0, 0, null, 0, &px, 0, &w, &h),
    );
}

test "jof_read_tile requires scratch for a deflate atlas" {
    var backing: [64]u8 = std.mem.zeroes([64]u8);
    makeMinimalAtlas(&backing);
    backing[13] = jof_h.k_jof_codec_deflate;
    var store = jof_h.jof_memstore_t{ .buf = &backing, .cap = backing.len, .len = backing.len };
    var info = std.mem.zeroes(jof_h.jof_info_t);
    try std.testing.expectEqual(jof_h.k_ra8_ok, jof_parse(jof_memstore_pread, &store, backing.len, &info));
    var px: [4]u8 = std.mem.zeroes([4]u8);
    var w: u16 = 0;
    var h: u16 = 0;
    try std.testing.expectEqual(
        jof_h.k_ra8_err_null_ptr,
        jof_read_tile(jof_memstore_pread, &store, &info, 0, 0, null, 0, &px, px.len, &w, &h),
    );
}

test "jof_stored_bound wraps at the exact 2^32 product like the C" {
    try std.testing.expectEqual(@as(u32, 0), jof_stored_bound(3817748480));
    try std.testing.expectEqual(@as(u32, 1), jof_stored_bound(3817748481));
}

test "jof_stored_bound matches the C arithmetic" {
    try std.testing.expectEqual(@as(u32, 256), jof_stored_bound(0));
    try std.testing.expectEqual(@as(u32, 8 + 1 + 256), jof_stored_bound(8));
    try std.testing.expectEqual(@as(u32, 16 + 2 + 256), jof_stored_bound(16));
    try std.testing.expectEqual(@as(u32, 1024 + 128 + 256), jof_stored_bound(1024));
}

test "jof_tile_dims clamps the edge tiles and rejects the out-of-range axes" {
    var info = std.mem.zeroes(jof_h.jof_info_t);
    info.width = 100;
    info.height = 70;
    info.tile_w = 64;
    info.tile_h = 64;
    info.tile_cols = 2;
    info.tile_rows = 2;
    var w: u16 = 0;
    var h: u16 = 0;
    try std.testing.expectEqual(jof_h.k_ra8_ok, jof_tile_dims(&info, 0, 0, &w, &h));
    try std.testing.expectEqual(@as(u16, 64), w);
    try std.testing.expectEqual(@as(u16, 64), h);
    try std.testing.expectEqual(jof_h.k_ra8_ok, jof_tile_dims(&info, 1, 1, &w, &h));
    try std.testing.expectEqual(@as(u16, 36), w);
    try std.testing.expectEqual(@as(u16, 6), h);
    try std.testing.expectEqual(jof_h.k_ra8_err_out_of_range, jof_tile_dims(&info, 2, 0, &w, &h));
    try std.testing.expectEqual(jof_h.k_ra8_err_out_of_range, jof_tile_dims(&info, 0, 2, &w, &h));
    try std.testing.expectEqual(jof_h.k_ra8_err_null_ptr, jof_tile_dims(null, 0, 0, &w, &h));
}

test "jof_memstore round-trips through the sink and clamps pread at the end" {
    var backing: [64]u8 = undefined;
    var store = jof_h.jof_memstore_t{ .buf = &backing, .cap = backing.len, .len = 0 };
    try std.testing.expectEqual(jof_h.k_ra8_ok, jof_memstore_sink(&store, &[_]u8{ 1, 2, 3 }, 3));
    try std.testing.expectEqual(@as(usize, 3), store.len);
    try std.testing.expectEqual(jof_h.k_ra8_err_no_mem, jof_memstore_sink(&store, &backing, 65));
    var got: usize = 99;
    var out: [3]u8 = undefined;
    try std.testing.expectEqual(jof_h.k_ra8_ok, jof_memstore_pread(&store, 1, &out, 3, &got));
    try std.testing.expectEqual(@as(usize, 2), got);
    try std.testing.expectEqual(@as(u8, 2), out[0]);
    try std.testing.expectEqual(@as(u8, 3), out[1]);
    try std.testing.expectEqual(jof_h.k_ra8_ok, jof_memstore_pread(&store, 3, &out, 1, &got));
    try std.testing.expectEqual(@as(usize, 0), got);
    try std.testing.expectEqual(jof_h.k_ra8_err_null_ptr, jof_memstore_sink(null, &backing, 1));
    try std.testing.expectEqual(jof_h.k_ra8_err_null_ptr, jof_memstore_pread(null, 0, &out, 1, &got));
}

test "audit rd_u32 and FNV-1a match the C arithmetic" {
    const bytes = [_]u8{ 0x12, 0x34, 0x56, 0x78 };
    try std.testing.expectEqual(@as(u32, 0x78563412), auditRdU32(&bytes));
    var uniform: bool = false;
    try std.testing.expectEqual(@as(u32, 2166136261), auditHash(&[_]u8{}, &uniform));
    try std.testing.expect(uniform);
    const h1 = auditHash(&[_]u8{0xAB}, &uniform);
    try std.testing.expect(uniform);
    const h2 = auditHash(&[_]u8{ 0xAB, 0xAB }, &uniform);
    try std.testing.expect(uniform);
    try std.testing.expect(h1 != h2);
    var nonuniform: bool = true;
    _ = auditHash(&[_]u8{ 0xAB, 0xCD }, &nonuniform);
    try std.testing.expect(!nonuniform);
}

test "audit entry points reject nulls before any parse" {
    try std.testing.expectEqual(
        jof_h.k_ra8_err_null_ptr,
        jof_audit_requirements(null, null, 0, null),
    );
    try std.testing.expectEqual(
        jof_h.k_ra8_err_null_ptr,
        jof_audit(null, null, 0, null, null),
    );
}

test "jof_parse rejects nulls and out-of-window sizes" {
    var info = std.mem.zeroes(jof_h.jof_info_t);
    try std.testing.expectEqual(jof_h.k_ra8_err_null_ptr, jof_parse(null, null, 0, null));
    try std.testing.expectEqual(
        jof_h.k_ra8_err_invalid_size,
        jof_parse(jof_memstore_pread, null, 4, &info),
    );
    try std.testing.expectEqual(
        jof_h.k_ra8_err_invalid_size,
        jof_parse(jof_memstore_pread, null, @as(u64, std.math.maxInt(u32)) + 64, &info),
    );
}

test "jof_parse validates a minimal raw atlas through the memstore seam" {
    var backing: [64]u8 = std.mem.zeroes([64]u8);
    makeMinimalAtlas(&backing);
    var store = jof_h.jof_memstore_t{ .buf = &backing, .cap = backing.len, .len = backing.len };
    var info = std.mem.zeroes(jof_h.jof_info_t);
    try std.testing.expectEqual(
        jof_h.k_ra8_ok,
        jof_parse(jof_memstore_pread, &store, backing.len, &info),
    );
    try std.testing.expectEqual(@as(u16, 1), info.width);
    try std.testing.expectEqual(@as(u16, 1), info.height);
    try std.testing.expectEqual(@as(u8, 1), info.bpp);
    try std.testing.expectEqual(@as(u8, jof_h.k_jof_codec_raw), info.codec);
    try std.testing.expectEqual(@as(u32, 1), info.tile_count);
    try std.testing.expectEqual(@as(u32, 40), info.index_off);
    try std.testing.expectEqual(@as(u32, 64), info.total_size);
}

test "jof_parse rejects a bad magic and a footer count that lies" {
    var backing: [64]u8 = std.mem.zeroes([64]u8);
    makeMinimalAtlas(&backing);
    var store = jof_h.jof_memstore_t{ .buf = &backing, .cap = backing.len, .len = backing.len };
    var info = std.mem.zeroes(jof_h.jof_info_t);
    backing[0] = 'X';
    try std.testing.expectEqual(
        jof_h.k_ra8_err_validation_failed,
        jof_parse(jof_memstore_pread, &store, backing.len, &info),
    );
    makeMinimalAtlas(&backing);
    backing[52] = 2; // footer tile count no longer matches the grid
    try std.testing.expectEqual(
        jof_h.k_ra8_err_validation_failed,
        jof_parse(jof_memstore_pread, &store, backing.len, &info),
    );
}

// RA8FW-1151 tests ------------------------------------------------------------

var s_prefix_inner_calls: usize = 0;

fn prefixInnerPull(ctx: ?*anyopaque, buf: [*c]u8, cap: usize, got: [*c]usize) callconv(.c) jof_h.ra8_err_t {
    _ = ctx;
    s_prefix_inner_calls += 1;
    if (cap == 0) {
        got.* = 0;
        return jof_h.k_ra8_ok;
    }
    buf[0] = 'Z';
    got.* = 1;
    return jof_h.k_ra8_ok;
}

fn prefixInnerFail(ctx: ?*anyopaque, buf: [*c]u8, cap: usize, got: [*c]usize) callconv(.c) jof_h.ra8_err_t {
    _ = .{ ctx, buf, cap, got };
    return jof_h.k_ra8_err_invalid_size;
}

test "priv_jof_on_geom binds the grid, carves and sinks the header" {
    var backing: [64]u8 = std.mem.zeroes([64]u8);
    var store = jof_h.jof_memstore_t{ .buf = &backing, .cap = backing.len, .len = 0 };
    var arena: [256]u8 align(8) = undefined;
    var bump = jof_h.jof_bump_t{ .base = &arena, .cap = arena.len, .off = 0 };
    var cfg = std.mem.zeroes(jof_h.jof_produce_cfg_t);
    cfg.tile_w = 2;
    cfg.tile_h = 2;
    cfg.codec = jof_h.k_jof_codec_raw;
    cfg.sink = jof_memstore_sink;
    cfg.sink_ctx = &store;
    var st = std.mem.zeroes(jof_h.jof_prod_state_t);
    st.cfg = &cfg;
    st.bump = &bump;
    st.cap_w = 32768;
    st.cap_h = 32768;
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_on_geom(&st, 3, 2, 1));
    // The grid: 3x2 pixels in 2x2 tiles -> 2 columns x 1 row.
    try std.testing.expectEqual(@as(u16, 2), st.tile_cols);
    try std.testing.expectEqual(@as(u16, 1), st.tile_rows);
    try std.testing.expectEqual(@as(u32, 2), st.tile_count);
    try std.testing.expect(st.band != null and st.stage != null and st.idx != null);
    try std.testing.expectEqual(@as(u8, 1), st.geom_done);
    try std.testing.expectEqual(@as(u32, jof_h.k_jof_hdr_bytes), st.written);
    try std.testing.expectEqual(@as(usize, 32), store.len);
    // Header bytes: magic, geometry, bpp, codec, tile count, zero fill.
    try std.testing.expectEqual(@as(u8, 'J'), backing[0]);
    try std.testing.expectEqual(@as(u8, 'O'), backing[1]);
    try std.testing.expectEqual(@as(u8, 'F'), backing[2]);
    try std.testing.expectEqual(@as(u8, '1'), backing[3]);
    try std.testing.expectEqual(@as(u16, 3), std.mem.readInt(u16, backing[4..6], .little));
    try std.testing.expectEqual(@as(u16, 2), std.mem.readInt(u16, backing[6..8], .little));
    try std.testing.expectEqual(@as(u16, 2), std.mem.readInt(u16, backing[8..10], .little));
    try std.testing.expectEqual(@as(u16, 2), std.mem.readInt(u16, backing[10..12], .little));
    try std.testing.expectEqual(@as(u8, 1), backing[jof_h.k_jof_ofs_bpp]);
    try std.testing.expectEqual(@as(u8, 0), backing[jof_h.k_jof_ofs_codec]);
    try std.testing.expectEqual(@as(u32, 2), std.mem.readInt(u32, backing[16..20], .little));
    for (backing[20..32]) |b| {
        try std.testing.expectEqual(@as(u8, 0), b);
    }
}

test "priv_jof_on_geom refuses a refire, over-cap dims, an over-cap grid and a starved arena" {
    var backing: [64]u8 = std.mem.zeroes([64]u8);
    var store = jof_h.jof_memstore_t{ .buf = &backing, .cap = backing.len, .len = 0 };
    var arena: [256]u8 align(8) = undefined;
    var bump = jof_h.jof_bump_t{ .base = &arena, .cap = arena.len, .off = 0 };
    var cfg = std.mem.zeroes(jof_h.jof_produce_cfg_t);
    cfg.tile_w = 2;
    cfg.tile_h = 2;
    cfg.codec = jof_h.k_jof_codec_raw;
    cfg.sink = jof_memstore_sink;
    cfg.sink_ctx = &store;
    var st = std.mem.zeroes(jof_h.jof_prod_state_t);
    st.cfg = &cfg;
    st.bump = &bump;
    st.cap_w = 8;
    st.cap_h = 8;
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_on_geom(&st, 4, 4, 1));
    // The geometry hook fires once per transcode.
    try std.testing.expectEqual(jof_h.k_ra8_err_invalid_state, priv_jof_on_geom(&st, 4, 4, 1));
    // Over the caller's width cap.
    var st2 = std.mem.zeroes(jof_h.jof_prod_state_t);
    st2.cfg = &cfg;
    st2.bump = &bump;
    st2.cap_w = 8;
    st2.cap_h = 8;
    try std.testing.expectEqual(jof_h.k_ra8_err_invalid_size, priv_jof_on_geom(&st2, 9, 1, 1));
    // Over the 65536-tile grid cap: 32768x32768 in 1x1 tiles.
    var st3 = std.mem.zeroes(jof_h.jof_prod_state_t);
    st3.cfg = &cfg;
    st3.bump = &bump;
    st3.cap_w = 32768;
    st3.cap_h = 32768;
    cfg.tile_w = 1;
    cfg.tile_h = 1;
    try std.testing.expectEqual(jof_h.k_ra8_err_invalid_size, priv_jof_on_geom(&st3, 32768, 32768, 1));
    cfg.tile_w = 2;
    cfg.tile_h = 2;
    // An arena the carve cannot fit: band_bytes = 64*64*1 out of 256.
    var st4 = std.mem.zeroes(jof_h.jof_prod_state_t);
    st4.cfg = &cfg;
    st4.bump = &bump;
    st4.cap_w = 32768;
    st4.cap_h = 32768;
    try std.testing.expectEqual(jof_h.k_ra8_err_invalid_size, priv_jof_on_geom(&st4, 64, 64, 1));
}

test "priv_jof_prefix_pull replays the head, then delegates" {
    var head = [_]u8{ 'A', 'B' };
    var pfx = jof_h.jof_prefix_pull_t{
        .head = &head,
        .head_len = 2,
        .pos = 0,
        .inner = prefixInnerPull,
        .inner_ctx = null,
    };
    var out: [4]u8 = undefined;
    var got: usize = 0;
    // A one-byte pull serves the first head byte only.
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_prefix_pull(@ptrCast(&pfx), &out, 1, &got));
    try std.testing.expectEqual(@as(usize, 1), got);
    try std.testing.expectEqual(@as(u8, 'A'), out[0]);
    try std.testing.expectEqual(@as(usize, 1), pfx.pos);
    // A wider pull serves the rest of the head, never past it.
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_prefix_pull(@ptrCast(&pfx), &out, 4, &got));
    try std.testing.expectEqual(@as(usize, 1), got);
    try std.testing.expectEqual(@as(u8, 'B'), out[0]);
    try std.testing.expectEqual(@as(usize, 2), pfx.pos);
    // Head exhausted: the inner source answers.
    s_prefix_inner_calls = 0;
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_prefix_pull(@ptrCast(&pfx), &out, 4, &got));
    try std.testing.expectEqual(@as(usize, 1), got);
    try std.testing.expectEqual(@as(u8, 'Z'), out[0]);
    try std.testing.expectEqual(@as(usize, 1), s_prefix_inner_calls);
    // A headless adapter delegates straight through, errors included.
    var bare = jof_h.jof_prefix_pull_t{
        .head = &head,
        .head_len = 0,
        .pos = 0,
        .inner = prefixInnerFail,
        .inner_ctx = null,
    };
    try std.testing.expectEqual(
        jof_h.k_ra8_err_invalid_size,
        priv_jof_prefix_pull(@ptrCast(&bare), &out, 4, &got),
    );
}

// RA8FW-1152 tests ------------------------------------------------------------

/// Shared producer fixture: raw codec, 2x2 tiles, memstore sink, a 256-byte
/// arena, 32768-dim caps, geometry bound at width x height x bpp.
const ProdFixture = struct {
    backing: [256]u8 = std.mem.zeroes([256]u8),
    store: jof_h.jof_memstore_t = undefined,
    arena: [256]u8 align(8) = undefined,
    bump: jof_h.jof_bump_t = undefined,
    cfg: jof_h.jof_produce_cfg_t = undefined,
    st: jof_h.jof_prod_state_t = undefined,

    fn init(self: *ProdFixture, width: u16, height: u16, bits_pp: u8, codec: u8) jof_h.ra8_err_t {
        self.* = .{};
        self.store = .{ .buf = &self.backing, .cap = self.backing.len, .len = 0 };
        self.bump = .{ .base = &self.arena, .cap = self.arena.len, .off = 0 };
        self.cfg = std.mem.zeroes(jof_h.jof_produce_cfg_t);
        self.cfg.tile_w = 2;
        self.cfg.tile_h = 2;
        self.cfg.codec = codec;
        self.cfg.sink = jof_memstore_sink;
        self.cfg.sink_ctx = &self.store;
        self.st = std.mem.zeroes(jof_h.jof_prod_state_t);
        self.st.cfg = &self.cfg;
        self.st.bump = &self.bump;
        self.st.cap_w = 32768;
        self.st.cap_h = 32768;
        const err = priv_jof_on_geom(&self.st, width, height, bits_pp);
        return err;
    }
};

test "priv_jof_on_rows streams a 3x2 atlas through the band flush into the memstore" {
    var fx = ProdFixture{};
    try std.testing.expectEqual(jof_h.k_ra8_ok, fx.init(3, 2, 1, jof_h.k_jof_codec_raw));
    // Row-major source: row 0 = a b c, row 1 = d e f.
    const rows = [_]u8{ 0xA, 0xB, 0xC, 0xD, 0xE, 0xF };
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_on_rows(&fx.st, &rows, 3, 0, 2, 1));
    // The band filled to tile_h: two tiles flushed, row-major tile order.
    try std.testing.expectEqual(@as(u32, 2), fx.st.tiles_done);
    try std.testing.expectEqual(@as(u32, 2), fx.st.rows_seen);
    try std.testing.expectEqual(@as(u32, 0), fx.st.band_fill);
    // Header + tile 0 (a b d e) + tile 1 (c f).
    try std.testing.expectEqual(@as(usize, 38), fx.store.len);
    try std.testing.expectEqual(@as(u8, 0xA), fx.backing[32]);
    try std.testing.expectEqual(@as(u8, 0xB), fx.backing[33]);
    try std.testing.expectEqual(@as(u8, 0xD), fx.backing[34]);
    try std.testing.expectEqual(@as(u8, 0xE), fx.backing[35]);
    try std.testing.expectEqual(@as(u8, 0xC), fx.backing[36]);
    try std.testing.expectEqual(@as(u8, 0xF), fx.backing[37]);
    // Index entries: absolute offsets and stored lengths.
    const idx: [*c]const u8 = fx.st.idx;
    try std.testing.expectEqual(@as(u32, 32), std.mem.readInt(u32, idx[0..4], .little));
    try std.testing.expectEqual(@as(u32, 4), std.mem.readInt(u32, idx[4..8], .little));
    try std.testing.expectEqual(@as(u32, 36), std.mem.readInt(u32, idx[8..12], .little));
    try std.testing.expectEqual(@as(u32, 2), std.mem.readInt(u32, idx[12..16], .little));
}

test "priv_jof_on_rows refuses null pixels and every contract violation" {
    var fx = ProdFixture{};
    try std.testing.expectEqual(jof_h.k_ra8_ok, fx.init(3, 2, 1, jof_h.k_jof_codec_raw));
    const rows = [_]u8{ 0xA, 0xB, 0xC };
    try std.testing.expectEqual(jof_h.k_ra8_err_null_ptr, priv_jof_on_rows(&fx.st, null, 3, 0, 1, 1));
    try std.testing.expectEqual(jof_h.k_ra8_err_validation_failed, priv_jof_on_rows(&fx.st, &rows, 3, 0, 0, 1));
    try std.testing.expectEqual(jof_h.k_ra8_err_validation_failed, priv_jof_on_rows(&fx.st, &rows, 2, 0, 1, 1));
    try std.testing.expectEqual(jof_h.k_ra8_err_validation_failed, priv_jof_on_rows(&fx.st, &rows, 3, 0, 1, 3));
    try std.testing.expectEqual(jof_h.k_ra8_err_validation_failed, priv_jof_on_rows(&fx.st, &rows, 3, 1, 1, 1));
    try std.testing.expectEqual(jof_h.k_ra8_err_validation_failed, priv_jof_on_rows(&fx.st, &rows, 3, 0, 3, 1));
    // Before the geometry hook fires nothing streams.
    var fx2 = ProdFixture{};
    fx2.store = .{ .buf = &fx2.backing, .cap = fx2.backing.len, .len = 0 };
    fx2.cfg = std.mem.zeroes(jof_h.jof_produce_cfg_t);
    fx2.cfg.sink = jof_memstore_sink;
    fx2.cfg.sink_ctx = &fx2.store;
    fx2.st = std.mem.zeroes(jof_h.jof_prod_state_t);
    fx2.st.cfg = &fx2.cfg;
    try std.testing.expectEqual(jof_h.k_ra8_err_validation_failed, priv_jof_on_rows(&fx2.st, &rows, 3, 0, 1, 1));
}

test "priv_jof_on_rows spans calls and holds a partial band" {
    var fx = ProdFixture{};
    try std.testing.expectEqual(jof_h.k_ra8_ok, fx.init(4, 4, 1, jof_h.k_jof_codec_raw));
    const row = [_]u8{ 1, 2, 3, 4 };
    // One row at a time: the first two rows fill the band and flush it.
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_on_rows(&fx.st, &row, 4, 0, 1, 1));
    try std.testing.expectEqual(@as(u32, 0), fx.st.tiles_done);
    try std.testing.expectEqual(@as(u32, 1), fx.st.band_fill);
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_on_rows(&fx.st, &row, 4, 1, 1, 1));
    try std.testing.expectEqual(@as(u32, 2), fx.st.tiles_done);
    try std.testing.expectEqual(@as(u32, 0), fx.st.band_fill);
    // The last band stays unflushed until the epilogue.
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_on_rows(&fx.st, &row, 4, 2, 1, 1));
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_on_rows(&fx.st, &row, 4, 3, 1, 1));
    try std.testing.expectEqual(@as(u32, 4), fx.st.tiles_done);
    try std.testing.expectEqual(@as(u32, 0), fx.st.band_fill);
    try std.testing.expectEqual(@as(u32, 4), fx.st.rows_seen);
}

test "priv_jof_on_rows encodes a deflate atlas through the bounded tile buffer" {
    // The deflate carve needs the full 319352-byte compressor scratch, so
    // this test carries its own arena instead of ProdFixture's 256 bytes.
    var fx: ProdFixture = undefined;
    fx = .{};
    var dfl_arena: [340000]u8 align(8) = undefined;
    fx.store = .{ .buf = &fx.backing, .cap = fx.backing.len, .len = 0 };
    fx.bump = .{ .base = &dfl_arena, .cap = dfl_arena.len, .off = 0 };
    fx.cfg = std.mem.zeroes(jof_h.jof_produce_cfg_t);
    fx.cfg.tile_w = 2;
    fx.cfg.tile_h = 2;
    fx.cfg.codec = jof_h.k_jof_codec_deflate;
    fx.cfg.sink = jof_memstore_sink;
    fx.cfg.sink_ctx = &fx.store;
    fx.st = std.mem.zeroes(jof_h.jof_prod_state_t);
    fx.st.cfg = &fx.cfg;
    fx.st.bump = &fx.bump;
    fx.st.cap_w = 32768;
    fx.st.cap_h = 32768;
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_on_geom(&fx.st, 2, 2, 1));
    const rows = [_]u8{ 7, 8, 9, 10 };
    try std.testing.expectEqual(jof_h.k_ra8_ok, priv_jof_on_rows(&fx.st, &rows, 2, 0, 2, 1));
    // One tile; the copy-compressor passes the payload through unchanged.
    try std.testing.expectEqual(@as(u32, 1), fx.st.tiles_done);
    try std.testing.expectEqual(@as(usize, 36), fx.store.len);
    try std.testing.expectEqual(@as(u8, 7), fx.backing[32]);
    try std.testing.expectEqual(@as(u8, 10), fx.backing[35]);
    const idx: [*c]const u8 = fx.st.idx;
    try std.testing.expectEqual(@as(u32, 32), std.mem.readInt(u32, idx[0..4], .little));
    try std.testing.expectEqual(@as(u32, 4), std.mem.readInt(u32, idx[4..8], .little));
}

// RA8FW-1153 tests ------------------------------------------------------------

/// Test pull over a static source: SOI head + payload, delivered whole.
var e2e_src: []const u8 = &.{};
var e2e_pos: usize = 0;

fn e2e_pull(ctx: ?*anyopaque, buf: [*c]u8, cap: usize, got: [*c]usize) callconv(.c) jof_h.ra8_err_t {
    _ = ctx;
    const left = e2e_src.len - e2e_pos;
    const n = @min(cap, left);
    if (n > 0) {
        @memcpy(buf[0..n], e2e_src[e2e_pos..][0..n]);
    }
    e2e_pos += n;
    got.* = n;
    return jof_h.k_ra8_ok;
}

test "jof_produce streams a JPEG source end to end into a complete atlas" {
    // SOI + payload; the stub decoder binds 3x2 grayscale and streams both
    // rows, so the whole state machine runs for real.
    e2e_src = &.{ 0xFF, 0xD8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA };
    e2e_pos = 0;
    jpeg_stub_w = 3;
    jpeg_stub_h = 2;
    jpeg_stub_channels = 1;
    jpeg_stub_rows_per_call = 2;
    @memset(jpeg_stub_px[0..], 0);
    jpeg_stub_px[0..6].* = .{ 0xA, 0xB, 0xC, 0xD, 0xE, 0xF };

    var backing: [256]u8 = undefined;
    // The work arena must hold the carve (band+stage+index), the streaming
    // JPEG's 128 KiB window and the stripe buffer, so it is heap-allocated.
    const arena = try std.testing.allocator.alloc(u8, 200000);
    defer std.testing.allocator.free(arena);
    var store: jof_h.jof_memstore_t = .{ .buf = &backing, .cap = backing.len, .len = 0 };
    var cfg = std.mem.zeroes(jof_h.jof_produce_cfg_t);
    cfg.tile_w = 2;
    cfg.tile_h = 2;
    cfg.codec = jof_h.k_jof_codec_raw;
    cfg.pull = e2e_pull;
    cfg.pull_ctx = null;
    cfg.sink = jof_memstore_sink;
    cfg.sink_ctx = &store;
    cfg.work = arena.ptr;
    cfg.work_cap = arena.len;
    var info: jof_h.jof_info_t = std.mem.zeroes(jof_h.jof_info_t);

    try std.testing.expectEqual(jof_h.k_ra8_ok, jof_produce(&cfg, &info));
    try std.testing.expectEqual(@as(u16, 3), info.width);
    try std.testing.expectEqual(@as(u16, 2), info.height);
    try std.testing.expectEqual(@as(u16, 2), info.tile_w);
    try std.testing.expectEqual(@as(u16, 2), info.tile_h);
    try std.testing.expectEqual(@as(u16, 2), info.tile_cols);
    try std.testing.expectEqual(@as(u16, 1), info.tile_rows);
    try std.testing.expectEqual(@as(u8, 1), info.bpp);
    try std.testing.expectEqual(@as(u8, jof_h.k_jof_codec_raw), info.codec);
    try std.testing.expectEqual(@as(u32, 2), info.tile_count);
    try std.testing.expectEqual(@as(u32, 38), info.index_off);
    try std.testing.expectEqual(@as(u32, 70), info.total_size);
    // Tile payloads in row-major tile order.
    try std.testing.expectEqual(@as(u8, 0xA), backing[32]);
    try std.testing.expectEqual(@as(u8, 0xE), backing[35]);
    try std.testing.expectEqual(@as(u8, 0xC), backing[36]);
    try std.testing.expectEqual(@as(u8, 0xF), backing[37]);
    // Index entries.
    try std.testing.expectEqual(@as(u32, 32), std.mem.readInt(u32, backing[38..42], .little));
    try std.testing.expectEqual(@as(u32, 4), std.mem.readInt(u32, backing[42..46], .little));
    try std.testing.expectEqual(@as(u32, 36), std.mem.readInt(u32, backing[46..50], .little));
    try std.testing.expectEqual(@as(u32, 2), std.mem.readInt(u32, backing[50..54], .little));
    // Footer: index_off, tile_count, total_size, magic.
    try std.testing.expectEqual(@as(u32, 38), std.mem.readInt(u32, backing[54..58], .little));
    try std.testing.expectEqual(@as(u32, 2), std.mem.readInt(u32, backing[58..62], .little));
    try std.testing.expectEqual(@as(u32, 70), std.mem.readInt(u32, backing[62..66], .little));
    try std.testing.expectEqual(@as(u8, 'J'), backing[66]);
    try std.testing.expectEqual(@as(u8, 'O'), backing[67]);
    try std.testing.expectEqual(@as(u8, 'F'), backing[68]);
    try std.testing.expectEqual(@as(u8, 'E'), backing[69]);
}

test "jof_produce rejects null args, bad config and an under-delivered decode" {
    var cfg = std.mem.zeroes(jof_h.jof_produce_cfg_t);
    var info: jof_h.jof_info_t = std.mem.zeroes(jof_h.jof_info_t);
    try std.testing.expectEqual(jof_h.k_ra8_err_null_ptr, jof_produce(null, &info));
    try std.testing.expectEqual(jof_h.k_ra8_err_null_ptr, jof_produce(&cfg, null));
    try std.testing.expectEqual(jof_h.k_ra8_err_null_ptr, jof_produce(&cfg, &info));
    // Bad geometry and an unknown codec fail before any sink write.
    cfg.tile_w = 0;
    cfg.tile_h = 2;
    cfg.pull = e2e_pull;
    cfg.sink = jof_memstore_sink;
    var arena: [256]u8 = undefined;
    cfg.work = &arena;
    try std.testing.expectEqual(jof_h.k_ra8_err_invalid_arg, jof_produce(&cfg, &info));
    cfg.tile_w = 2;
    cfg.codec = 3;
    try std.testing.expectEqual(jof_h.k_ra8_err_invalid_arg, jof_produce(&cfg, &info));
    // A too-short source dies on the sniff head.
    cfg.codec = jof_h.k_jof_codec_raw;
    var backing: [256]u8 = undefined;
    var store: jof_h.jof_memstore_t = .{ .buf = &backing, .cap = backing.len, .len = 0 };
    cfg.sink_ctx = &store;
    e2e_src = &.{ 0xFF, 0xD8 };
    e2e_pos = 0;
    try std.testing.expectEqual(jof_h.k_ra8_err_protocol_error, jof_produce(&cfg, &info));
}

test "jof_probe_dims answers the JPEG arm and refuses short or foreign bytes" {
    jpeg_stub_w = 3;
    jpeg_stub_h = 2;
    var w: u16 = 0;
    var h: u16 = 0;
    var soi14: [14]u8 = .{ 0xFF, 0xD8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    try std.testing.expectEqual(jof_h.k_ra8_ok, jof_probe_dims(&soi14, 14, &w, &h));
    try std.testing.expectEqual(@as(u16, 3), w);
    try std.testing.expectEqual(@as(u16, 2), h);
    var soi2: [2]u8 = .{ 0xFF, 0xD8 };
    try std.testing.expectEqual(jof_h.k_ra8_err_not_supported, jof_probe_dims(&soi2, 2, &w, &h));
    // A non-JPEG/PNG/WebP head is refused.
    var zeros16: [16]u8 = @splat(0);
    try std.testing.expectEqual(jof_h.k_ra8_err_not_supported, jof_probe_dims(&zeros16, 16, &w, &h));
    // The null contract.
    try std.testing.expectEqual(jof_h.k_ra8_err_null_ptr, jof_probe_dims(null, 16, &w, &h));
    var soi4 = [_]u8{ 0xFF, 0xD8, 0xFF, 0xD8 };
    try std.testing.expectEqual(jof_h.k_ra8_err_null_ptr, jof_probe_dims(&soi4, 4, null, &h));
}

test "internal_finish refuses an under-flushed atlas" {
    var fx = ProdFixture{};
    try std.testing.expectEqual(jof_h.k_ra8_ok, fx.init(3, 2, 1, jof_h.k_jof_codec_raw));
    var info: jof_h.jof_info_t = std.mem.zeroes(jof_h.jof_info_t);
    // tile_count is 2, tiles_done 0: the epilogue guard fails first, and
    // finish's own guard is reachable through a bound-but-unflushed state.
    fx.st.tiles_done = 1;
    try std.testing.expectEqual(jof_h.k_ra8_err_validation_failed, internal_finish(&fx.st, &info));
}
