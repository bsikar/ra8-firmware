//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig implementation of `longstrip` (RA8FW-1156): the continuous
//! vertical-scroll engine over a JOF band-tile atlas. The C ABI surface in
//! `inc/longstrip.h` is unchanged; CMake and the reader module consume the
//! installed static library through that header. Types come from the
//! translated `longstrip_h` seam so every layout is the compiler's word, and
//! the JOF reads and tile-cache calls run through their C ABI exactly as the
//! C TU ran them.

const std = @import("std");
const ls_h = @import("longstrip_h");

/// Component tag for `RA8_CHECK_*` log lines.
const s_tag: [*c]const u8 = "longstrip";

// wt_consts_t: fixed engine constants (no magic numbers). The fling ratio
// decays velocity 7/8 per tick toward zero; a longstrip strip has exactly one
// band column, read at native zoom.
const k_wt_fling_num: i32 = 7;
const k_wt_fling_den: i32 = 8;
const k_wt_tile_x: u16 = 0;
const k_wt_zoom: u16 = 0;

// longstrip_limits_t: fixed engine bounds. The render loop is additionally
// bounded by band_count; the prefetch loop by k_longstrip_max_prefetch.
pub const k_longstrip_max_visible_bands: u16 = 4096;
pub const k_longstrip_max_prefetch: u16 = 8;

// ---------------------------------------------------------------------------
// Seam externs: the jof library (its own Zig archive, C ABI), the tile
// cache, and the RA8_CHECK log backend (ra8_log.h).
// ---------------------------------------------------------------------------
extern fn ra8_log_emit_error(tag: [*c]const u8, message: [*c]const u8) callconv(.c) void;
extern fn jof_parse(pread: ls_h.jof_pread_fn, pread_ctx: ?*anyopaque, total_size: u64, out_info: ?*ls_h.jof_info_t) callconv(.c) ls_h.ra8_err_t;
extern fn jof_read_tile(pread: ls_h.jof_pread_fn, pread_ctx: ?*anyopaque, info: ?*const ls_h.jof_info_t, tile_x: u16, tile_y: u16, scratch: ?[*]u8, scratch_cap: u32, out_px: ?[*]u8, out_cap: u32, out_w: ?*u16, out_h: ?*u16) callconv(.c) ls_h.ra8_err_t;
extern fn ra8_tile_cache_get(tc: ?*ls_h.ra8_tile_cache_t, key: ?*const ls_h.ra8_tile_key_t, out_tile: ?*ls_h.ra8_tile_t) callconv(.c) ls_h.ra8_err_t;
extern fn ra8_tile_cache_put(tc: ?*ls_h.ra8_tile_cache_t, pixels: ?*const u8) callconv(.c) ls_h.ra8_err_t;

/// The RA8_CHECK_NULL_PTR macro's body: log through the ra8_error backend and
/// fail closed with k_ra8_err_null_ptr.
fn null_check(msg: [*c]const u8, ptr: anytype) bool {
    if (ptr == null) {
        ra8_log_emit_error(s_tag, msg);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// longstrip_tile_decode: the decode-on-miss seam the tile cache drives.
// ---------------------------------------------------------------------------
pub export fn longstrip_tile_decode(ctx: ?*anyopaque, key: ?*const ls_h.ra8_tile_key_t, cell: ?[*]u8, cell_bytes: u32, out_w: ?*u16, out_h: ?*u16) callconv(.c) ls_h.ra8_err_t {
    if (!null_check("decode ctx must not be nullptr", ctx)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("key must not be nullptr", key)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("cell must not be nullptr", cell)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("out_w must not be nullptr", out_w)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("out_h must not be nullptr", out_h)) return ls_h.k_ra8_err_null_ptr;
    const dc: *ls_h.longstrip_decode_ctx_t = @ptrCast(@alignCast(ctx.?));
    return jof_read_tile(
        dc.pread,
        dc.pread_ctx,
        &dc.info,
        key.?.tile_x,
        key.?.tile_y,
        dc.scratch,
        dc.scratch_cap,
        cell.?,
        cell_bytes,
        out_w.?,
        out_h.?,
    );
}

/// Bind the parsed atlas geometry and config into the engine state: the
/// state-population half of longstrip_open. Homes the scroll to the top and
/// derives the max-scroll clamp (a strip no taller than the viewport cannot
/// scroll, so the clamp floors at zero).
fn internal_bind(wt: *ls_h.longstrip_t, cfg: *const ls_h.longstrip_cfg_t, info: *const ls_h.jof_info_t) void {
    wt.info = info.*;
    wt.canvas_w = @intCast(info.width);
    wt.canvas_h = @intCast(info.height);
    wt.band_h = @intCast(info.tile_h);
    wt.band_count = @intCast(info.tile_rows);
    wt.viewport_w = cfg.viewport_w;
    wt.viewport_h = cfg.viewport_h;
    wt.scroll_y = 0;
    wt.max_scroll = if (info.height > @as(u32, cfg.viewport_h))
        @intCast(info.height - @as(u32, cfg.viewport_h))
    else
        0;
    wt.velocity = 0;
    wt.cache = cfg.cache;
    wt.image_id = cfg.image_id;
    wt.blit = cfg.blit;
    wt.blit_ctx = cfg.blit_ctx;
}

/// Reject a NULL engine, config, or any NULL callback / cache it carries: the
/// null-guard half of longstrip_open.
fn internal_check_ptrs(wt: ?*const ls_h.longstrip_t, cfg: ?*const ls_h.longstrip_cfg_t) ls_h.ra8_err_t {
    if (!null_check("wt must not be nullptr", wt)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("cfg must not be nullptr", cfg)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("cfg->pread must not be nullptr", cfg.?.pread)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("cfg->cache must not be nullptr", cfg.?.cache)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("cfg->blit must not be nullptr", cfg.?.blit)) return ls_h.k_ra8_err_null_ptr;
    return ls_h.k_ra8_ok;
}

pub export fn longstrip_open(wt: ?*ls_h.longstrip_t, cfg: ?*const ls_h.longstrip_cfg_t) callconv(.c) ls_h.ra8_err_t {
    const ptr_err = internal_check_ptrs(wt, cfg);
    if (ptr_err != ls_h.k_ra8_ok) {
        return ptr_err;
    }
    // Reject a zero-area viewport (2 conditions -- MC/DC in tests).
    if ((cfg.?.viewport_w == 0) or (cfg.?.viewport_h == 0)) {
        return ls_h.k_ra8_err_invalid_arg;
    }
    var info = std.mem.zeroes(ls_h.jof_info_t);
    const err = jof_parse(cfg.?.pread, cfg.?.pread_ctx, cfg.?.atlas_size, &info);
    if (err != ls_h.k_ra8_ok) {
        return err;
    }
    // A longstrip strip is one full-width band column; anything else is not a
    // scrollable strip. jof_parse derives tile_cols as ceil(width / tile_w),
    // so tile_w == width already implies tile_cols == 1 -- the full-width test
    // alone is necessary and sufficient.
    if (@as(u32, info.tile_w) != info.width) {
        return ls_h.k_ra8_err_not_supported;
    }
    internal_bind(wt.?, cfg.?, &info);
    return ls_h.k_ra8_ok;
}

pub export fn longstrip_set_viewport(wt: ?*ls_h.longstrip_t, viewport_w: u16, viewport_h: u16) callconv(.c) ls_h.ra8_err_t {
    if (!null_check("wt must not be nullptr", wt)) return ls_h.k_ra8_err_null_ptr;
    // Reject a zero-area viewport (2 conditions -- MC/DC in tests).
    if ((viewport_w == 0) or (viewport_h == 0)) {
        return ls_h.k_ra8_err_invalid_arg;
    }
    const w = wt.?;
    w.viewport_w = viewport_w;
    w.viewport_h = viewport_h;
    w.max_scroll = if (w.canvas_h > @as(u32, viewport_h))
        @intCast(w.canvas_h - @as(u32, viewport_h))
    else
        0;
    w.scroll_y = longstrip_clamp_scroll(w, w.scroll_y);
    return ls_h.k_ra8_ok;
}

pub export fn longstrip_clamp_scroll(wt: ?*const ls_h.longstrip_t, y: i32) callconv(.c) i32 {
    if (wt == null) {
        return 0;
    }
    var yy = y;
    if (yy < 0) {
        yy = 0;
    } else if (yy > wt.?.max_scroll) {
        yy = wt.?.max_scroll;
    }
    return yy;
}

pub export fn longstrip_band_at_y(wt: ?*const ls_h.longstrip_t, y: u32, out_band: ?*u16) callconv(.c) ls_h.ra8_err_t {
    if (!null_check("wt must not be nullptr", wt)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("out_band must not be nullptr", out_band)) return ls_h.k_ra8_err_null_ptr;
    const w = wt.?;
    if (y >= w.canvas_h) {
        return ls_h.k_ra8_err_out_of_range;
    }
    out_band.?.* = @intCast(y / @as(u32, w.band_h));
    return ls_h.k_ra8_ok;
}

pub export fn longstrip_visible_bands(wt: ?*const ls_h.longstrip_t, scroll_y: i32, out_first: ?*u16, out_last: ?*u16) callconv(.c) ls_h.ra8_err_t {
    if (!null_check("wt must not be nullptr", wt)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("out_first must not be nullptr", out_first)) return ls_h.k_ra8_err_null_ptr;
    if (!null_check("out_last must not be nullptr", out_last)) return ls_h.k_ra8_err_null_ptr;
    const w = wt.?;
    const top: u32 = @intCast(longstrip_clamp_scroll(w, scroll_y));
    const bh: u32 = @intCast(w.band_h);
    const first: u32 = top / bh;
    var bottom: u32 = top + @as(u32, w.viewport_h) - 1;
    if (bottom >= w.canvas_h) {
        bottom = w.canvas_h - 1;
    }
    var last: u32 = bottom / bh;
    if (last >= @as(u32, w.band_count)) {
        last = @as(u32, w.band_count) - 1;
    }
    out_first.?.* = @intCast(first);
    out_last.?.* = @intCast(last);
    return ls_h.k_ra8_ok;
}

/// Saturating signed 32-bit add: pins at the int32 limits, never wraps. The
/// sum widens to i64 so a hostile scroll delta cannot overflow before the
/// range clamp.
fn internal_sat_add(a: i32, b: i32) i32 {
    const sum = @as(i64, a) + @as(i64, b);
    if (sum > std.math.maxInt(i32)) {
        return std.math.maxInt(i32);
    }
    if (sum < std.math.minInt(i32)) {
        return std.math.minInt(i32);
    }
    return @intCast(sum);
}

pub export fn longstrip_scroll_by(wt: ?*ls_h.longstrip_t, delta: i32) callconv(.c) ls_h.ra8_err_t {
    if (!null_check("wt must not be nullptr", wt)) return ls_h.k_ra8_err_null_ptr;
    const w = wt.?;
    w.scroll_y = longstrip_clamp_scroll(w, internal_sat_add(w.scroll_y, delta));
    // Pinned at either end stops a running fling (2 conditions).
    if ((w.scroll_y == 0) or (w.scroll_y == w.max_scroll)) {
        w.velocity = 0;
    }
    return ls_h.k_ra8_ok;
}

pub export fn longstrip_fling(wt: ?*ls_h.longstrip_t, v0: i32) callconv(.c) ls_h.ra8_err_t {
    if (!null_check("wt must not be nullptr", wt)) return ls_h.k_ra8_err_null_ptr;
    wt.?.velocity = v0;
    return ls_h.k_ra8_ok;
}

/// Decay a velocity one friction step toward zero: integer proportional decay
/// v * num / den (7/8), truncating toward zero, so the magnitude strictly
/// decreases and small speeds snap to rest -- a fling always terminates in a
/// bounded number of ticks.
fn internal_apply_friction(v: i32) i32 {
    const scaled = @as(i64, v) * @as(i64, k_wt_fling_num);
    return @intCast(@divTrunc(scaled, @as(i64, k_wt_fling_den)));
}

/// Report whether the viewport is pinned at the top of the strip. The clamp
/// keeps scroll_y >= 0, so equality is the pinned case.
fn internal_at_top(wt: *const ls_h.longstrip_t) bool {
    return wt.scroll_y <= 0;
}

/// Report whether the viewport is pinned at the bottom of the strip. Mirror
/// of internal_at_top over the max_scroll end.
fn internal_at_bottom(wt: *const ls_h.longstrip_t) bool {
    return wt.scroll_y >= wt.max_scroll;
}

/// True when a fling has hit the boundary it is travelling toward. The caller
/// only reaches this with a non-zero velocity, so the sign alone decides which
/// end halts the fling: an upward fling stops only at the top, a downward one
/// only at the bottom.
fn internal_fling_should_stop(wt: *const ls_h.longstrip_t) bool {
    return if (wt.velocity < 0) internal_at_top(wt) else internal_at_bottom(wt);
}

pub export fn longstrip_tick(wt: ?*ls_h.longstrip_t) callconv(.c) bool {
    if (wt == null) {
        return false;
    }
    const w = wt.?;
    if (w.velocity == 0) {
        return false;
    }
    w.scroll_y = longstrip_clamp_scroll(w, internal_sat_add(w.scroll_y, w.velocity));
    w.velocity = internal_apply_friction(w.velocity);
    // Come to rest when the speed hits zero or a boundary is reached.
    if ((w.velocity == 0) or internal_fling_should_stop(w)) {
        w.velocity = 0;
        return false;
    }
    return true;
}

/// Fetch a band into the cache then release it (warm, no held pin): the
/// directional prefetch primitive. A get + immediate put makes the band
/// resident and MRU without holding a pin, so the tile cache's LRU still
/// bounds residency. A decode miss is best-effort, so any error is swallowed.
fn internal_warm_band(wt: *ls_h.longstrip_t, band: u16) void {
    var key = std.mem.zeroes(ls_h.ra8_tile_key_t);
    key.image_id = wt.image_id;
    key.tile_x = k_wt_tile_x;
    key.tile_y = band;
    key.zoom = k_wt_zoom;
    var tile = std.mem.zeroes(ls_h.ra8_tile_t);
    if (ra8_tile_cache_get(wt.cache, &key, &tile) == ls_h.k_ra8_ok) {
        _ = ra8_tile_cache_put(wt.cache, tile.pixels);
    }
}

pub export fn longstrip_prefetch(wt: ?*ls_h.longstrip_t, depth: u16) callconv(.c) ls_h.ra8_err_t {
    if (!null_check("wt must not be nullptr", wt)) return ls_h.k_ra8_err_null_ptr;
    const w = wt.?;
    var first: u16 = 0;
    var last: u16 = 0;
    const err = longstrip_visible_bands(w, w.scroll_y, &first, &last);
    if (err != ls_h.k_ra8_ok) {
        return err;
    }
    var d = depth;
    if (d > k_longstrip_max_prefetch) {
        d = k_longstrip_max_prefetch;
    }
    const visible_span: u16 = (last - first) + 1;
    // Nothing to prefetch with zero depth or a strip that fits (2 conditions).
    if ((d == 0) or (w.band_count <= visible_span)) {
        return ls_h.k_ra8_ok;
    }
    const downward = w.velocity >= 0;
    var i: u16 = 1;
    while (i <= d) : (i += 1) { // bounded by k_longstrip_max_prefetch
        var target: u16 = 0;
        if (downward) {
            const t: u32 = @as(u32, last) + @as(u32, i);
            if (t >= @as(u32, w.band_count)) {
                break;
            }
            target = @intCast(t);
        } else {
            if (first < i) {
                break;
            }
            target = first - i;
        }
        internal_warm_band(w, target);
    }
    return ls_h.k_ra8_ok;
}

/// Add the on-screen row span of one blitted band to the coverage. Intersects
/// the band's destination rows [dst_y, dst_y + band_h) with the viewport and
/// adds the overlap to covered_rows; bands are contiguous and non-overlapping,
/// so the sum is the exact covered-row total with no double counting.
fn internal_accumulate_coverage(wt: *const ls_h.longstrip_t, dst_y: i32, band_h: u16, stats: *ls_h.longstrip_render_stats_t) void {
    const top: i32 = if (dst_y > 0) dst_y else 0;
    var bot: i32 = dst_y + @as(i32, band_h);
    const vh: i32 = @intCast(wt.viewport_h);
    if (bot > vh) {
        bot = vh;
    }
    if (bot > top) {
        stats.covered_rows += @intCast(bot - top);
    }
}

/// Fetch, composite and release one visible band; update stats. A cache /
/// decode miss is recorded as a skip and the frame continues; a blit-sink or
/// cache-release failure is fatal and propagates.
fn internal_draw_band(wt: *ls_h.longstrip_t, band: u16, dst_x: i32, stats: *ls_h.longstrip_render_stats_t) ls_h.ra8_err_t {
    var key = std.mem.zeroes(ls_h.ra8_tile_key_t);
    key.image_id = wt.image_id;
    key.tile_x = k_wt_tile_x;
    key.tile_y = band;
    key.zoom = k_wt_zoom;
    var tile = std.mem.zeroes(ls_h.ra8_tile_t);
    if (ra8_tile_cache_get(wt.cache, &key, &tile) != ls_h.k_ra8_ok) {
        stats.skipped += 1; // record a gap; the frame still completes
        return ls_h.k_ra8_ok;
    }
    const dst_y: i32 = @as(i32, @intCast(@as(u32, band) * @as(u32, wt.band_h))) - wt.scroll_y;
    const berr = wt.blit.?(wt.blit_ctx, tile.pixels, tile.width, tile.height, wt.info.bpp, dst_x, dst_y);
    if (berr == ls_h.k_ra8_ok) {
        stats.bands_drawn += 1;
        internal_accumulate_coverage(wt, dst_y, tile.height, stats);
    } else {
        stats.skipped += 1;
    }
    const perr = ra8_tile_cache_put(wt.cache, tile.pixels);
    return if (berr != ls_h.k_ra8_ok) berr else perr;
}

pub export fn longstrip_render(wt: ?*ls_h.longstrip_t, stats: ?*ls_h.longstrip_render_stats_t) callconv(.c) ls_h.ra8_err_t {
    if (!null_check("wt must not be nullptr", wt)) return ls_h.k_ra8_err_null_ptr;
    const w = wt.?;
    var local = std.mem.zeroes(ls_h.longstrip_render_stats_t);
    w.scroll_y = longstrip_clamp_scroll(w, w.scroll_y);
    var first: u16 = 0;
    var last: u16 = 0;
    var err = longstrip_visible_bands(w, w.scroll_y, &first, &last);
    if (err != ls_h.k_ra8_ok) {
        return err;
    }
    const dst_x: i32 = @divTrunc(@as(i32, @intCast(w.viewport_w)) - @as(i32, @intCast(w.canvas_w)), 2);
    var b: u32 = @as(u32, first);
    while (b <= @as(u32, last)) : (b += 1) { // bounded by band_count
        err = internal_draw_band(w, @intCast(b), dst_x, &local);
        if (err != ls_h.k_ra8_ok) {
            break;
        }
    }
    if (stats != null) {
        stats.?.* = local;
    }
    return err;
}

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
