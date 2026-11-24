//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `longstrip_abi.zig`.

const std = @import("std");
const ls_h = @import("longstrip_h");
const sut = @import("longstrip_abi");
const stubs = @import("abi_stubs.zig");

comptime {
    _ = @import("abi_stubs.zig");
    _ = stubs.Exports;
}

const longstrip_band_at_y = sut.longstrip_band_at_y;
const longstrip_clamp_scroll = sut.longstrip_clamp_scroll;
const longstrip_fling = sut.longstrip_fling;
const longstrip_open = sut.longstrip_open;
const longstrip_prefetch = sut.longstrip_prefetch;
const longstrip_render = sut.longstrip_render;
const longstrip_scroll_by = sut.longstrip_scroll_by;
const longstrip_set_viewport = sut.longstrip_set_viewport;
const longstrip_tick = sut.longstrip_tick;
const longstrip_visible_bands = sut.longstrip_visible_bands;

// ---------------------------------------------------------------------------
// Zig unit tests. The JOF reader, tile cache and log backend are the
// deterministic fakes in tests/abi_stubs.zig; the blit sink is a recording
// test double. Control flags ride exported stub vars.
// ---------------------------------------------------------------------------
extern var ls_stub_multi_column: c_int;
extern var ls_stub_cache_miss: c_int;
extern var ls_stub_puts: c_int;

const BlitRec = struct {
    calls: usize = 0,
    last_y: i32 = 0,
};

var g_rec: BlitRec = .{};

fn test_blit(ctx: ?*anyopaque, pixels: ?*const u8, src_w: u16, src_h: u16, bpp: u8, dst_x: i32, dst_y: i32) callconv(.c) ls_h.ra8_err_t {
    _ = pixels;
    _ = src_w;
    _ = src_h;
    _ = bpp;
    _ = dst_x;
    const rec: *BlitRec = @ptrCast(@alignCast(ctx.?));
    rec.calls += 1;
    rec.last_y = dst_y;
    return ls_h.k_ra8_ok;
}

fn dummy_pread(ctx: ?*anyopaque, offset: u64, buf: ?[*]u8, len: usize, got: ?*usize) callconv(.c) ls_h.ra8_err_t {
    _ = ctx;
    _ = offset;
    _ = buf;
    _ = len;
    _ = got;
    unreachable;
}

const test_cache_ptr: ?*ls_h.ra8_tile_cache_t = @ptrFromInt(64);

fn open_ok() ls_h.longstrip_t {
    var wt = std.mem.zeroes(ls_h.longstrip_t);
    var cfg = std.mem.zeroes(ls_h.longstrip_cfg_t);
    cfg.pread = dummy_pread;
    cfg.cache = test_cache_ptr;
    cfg.image_id = 7;
    cfg.viewport_w = 200;
    cfg.viewport_h = 300;
    cfg.blit = test_blit;
    cfg.blit_ctx = @ptrCast(&g_rec);
    const err = longstrip_open(&wt, &cfg);
    std.testing.expectEqual(ls_h.k_ra8_ok, err) catch unreachable;
    return wt;
}

test "open rejects null engine and config" {
    try std.testing.expectEqual(ls_h.k_ra8_err_null_ptr, longstrip_open(null, null));
    var cfg = std.mem.zeroes(ls_h.longstrip_cfg_t);
    try std.testing.expectEqual(ls_h.k_ra8_err_null_ptr, longstrip_open(@ptrFromInt(16), &cfg));
}

test "open rejects zero-area viewport" {
    var wt = std.mem.zeroes(ls_h.longstrip_t);
    var cfg = std.mem.zeroes(ls_h.longstrip_cfg_t);
    cfg.pread = dummy_pread;
    cfg.cache = test_cache_ptr;
    cfg.blit = test_blit;
    cfg.viewport_h = 300;
    try std.testing.expectEqual(ls_h.k_ra8_err_invalid_arg, longstrip_open(&wt, &cfg));
    cfg.viewport_w = 200;
    cfg.viewport_h = 0;
    try std.testing.expectEqual(ls_h.k_ra8_err_invalid_arg, longstrip_open(&wt, &cfg));
}

test "open binds geometry and homes the scroll" {
    const wt = open_ok();
    try std.testing.expectEqual(@as(u32, 200), wt.canvas_w);
    try std.testing.expectEqual(@as(u32, 1000), wt.canvas_h);
    try std.testing.expectEqual(@as(u16, 100), wt.band_h);
    try std.testing.expectEqual(@as(u16, 10), wt.band_count);
    try std.testing.expectEqual(@as(i32, 700), wt.max_scroll);
    try std.testing.expectEqual(@as(i32, 0), wt.scroll_y);
    try std.testing.expectEqual(@as(i32, 0), wt.velocity);
}

test "open rejects a non full-width atlas" {
    ls_stub_multi_column = 1;
    defer ls_stub_multi_column = 0;
    var wt = std.mem.zeroes(ls_h.longstrip_t);
    var cfg = std.mem.zeroes(ls_h.longstrip_cfg_t);
    cfg.pread = dummy_pread;
    cfg.cache = test_cache_ptr;
    cfg.blit = test_blit;
    cfg.viewport_w = 100;
    cfg.viewport_h = 300;
    try std.testing.expectEqual(ls_h.k_ra8_err_not_supported, longstrip_open(&wt, &cfg));
}

test "clamp scroll bounds both ends" {
    const wt = open_ok();
    try std.testing.expectEqual(@as(i32, 0), longstrip_clamp_scroll(&wt, -5));
    try std.testing.expectEqual(@as(i32, 700), longstrip_clamp_scroll(&wt, 5000));
    try std.testing.expectEqual(@as(i32, 100), longstrip_clamp_scroll(&wt, 100));
    try std.testing.expectEqual(@as(i32, 0), longstrip_clamp_scroll(null, 100));
}

test "band_at_y maps and range-checks" {
    const wt = open_ok();
    var band: u16 = 0xff;
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_band_at_y(&wt, 0, &band));
    try std.testing.expectEqual(@as(u16, 0), band);
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_band_at_y(&wt, 450, &band));
    try std.testing.expectEqual(@as(u16, 4), band);
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_band_at_y(&wt, 999, &band));
    try std.testing.expectEqual(@as(u16, 9), band);
    try std.testing.expectEqual(ls_h.k_ra8_err_out_of_range, longstrip_band_at_y(&wt, 1000, &band));
}

test "visible bands straddle the viewport" {
    const wt = open_ok();
    var first: u16 = 0;
    var last: u16 = 0;
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_visible_bands(&wt, 450, &first, &last));
    try std.testing.expectEqual(@as(u16, 4), first);
    try std.testing.expectEqual(@as(u16, 7), last);
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_visible_bands(&wt, 999, &first, &last));
    try std.testing.expectEqual(@as(u16, 9), last);
}

test "scroll_by saturates and a pinned end stops the fling" {
    var wt = open_ok();
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_scroll_by(&wt, std.math.maxInt(i32)));
    try std.testing.expectEqual(@as(i32, 700), wt.scroll_y);
    try std.testing.expectEqual(@as(i32, 0), wt.velocity);
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_fling(&wt, 100));
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_scroll_by(&wt, 1));
    try std.testing.expectEqual(@as(i32, 0), wt.velocity);
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_scroll_by(&wt, std.math.minInt(i32)));
    try std.testing.expectEqual(@as(i32, 0), wt.scroll_y);
}

test "fling ticks terminate" {
    var wt = open_ok();
    // A fling faster than the whole strip lands pinned at the bottom on the
    // first tick: the pinned end halts it with zero ticks of motion.
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_fling(&wt, 800));
    try std.testing.expectEqual(false, longstrip_tick(&wt));
    try std.testing.expectEqual(@as(i32, 700), wt.scroll_y);
    try std.testing.expectEqual(@as(i32, 0), wt.velocity);
    // A moderate fling decays 7/8 per tick to rest mid-strip.
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_scroll_by(&wt, -700));
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_fling(&wt, 60));
    var ticks: usize = 0;
    while (longstrip_tick(&wt)) {
        ticks += 1;
        try std.testing.expect(ticks < 4096);
    }
    try std.testing.expect(ticks > 0);
    try std.testing.expectEqual(@as(i32, 0), wt.velocity);
    try std.testing.expect(wt.scroll_y > 0 and wt.scroll_y < 700);
    try std.testing.expectEqual(false, longstrip_tick(null));
}

test "render composites the visible range and counts coverage" {
    var wt = open_ok();
    g_rec = .{};
    var stats = std.mem.zeroes(ls_h.longstrip_render_stats_t);
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_scroll_by(&wt, 450));
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_render(&wt, &stats));
    try std.testing.expectEqual(@as(u32, 4), stats.bands_drawn);
    try std.testing.expectEqual(@as(u32, 0), stats.skipped);
    try std.testing.expectEqual(@as(u32, 300), stats.covered_rows);
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_render(&wt, null));
}

test "render records a miss as a skip and still completes" {
    var wt = open_ok();
    ls_stub_cache_miss = 1;
    defer ls_stub_cache_miss = 0;
    var stats = std.mem.zeroes(ls_h.longstrip_render_stats_t);
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_render(&wt, &stats));
    try std.testing.expectEqual(@as(u32, 0), stats.bands_drawn);
    try std.testing.expectEqual(@as(u32, 3), stats.skipped);
    try std.testing.expectEqual(@as(u32, 0), stats.covered_rows);
}

test "prefetch warms downward and skips a strip that fits" {
    var wt = open_ok();
    ls_stub_puts = 0;
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_prefetch(&wt, 3));
    try std.testing.expectEqual(@as(c_int, 3), ls_stub_puts);
    // Depth above the cap clamps; bands 3..8 available below the window.
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_prefetch(&wt, 64));
    try std.testing.expectEqual(@as(c_int, 10), ls_stub_puts);
    // A viewport as tall as the strip leaves nothing to prefetch.
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_set_viewport(&wt, 200, 1000));
    try std.testing.expectEqual(ls_h.k_ra8_ok, longstrip_prefetch(&wt, 3));
    try std.testing.expectEqual(@as(c_int, 10), ls_stub_puts);
}
