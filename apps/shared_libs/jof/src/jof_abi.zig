//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI `jof_produce.h`, `jof_internal.h` and the image-codec headers
//! publish, over the Zig implementation behind it.
//!
//! Every jof C TU still compiles from JOF_SOURCES; as functions port they are
//! deleted from the C and exported from here, and consumers link this archive
//! beside them (the ra8_fs pattern). The pure-math work sizer landed first
//! (RA8FW-1140); the streaming trio below is the whole of jof_produce_webp.c,
//! whose TU this retires.

const jof_h = @import("jof_h");

pub export fn jof_webp_work_bytes(max_width: u16, max_height: u16, max_src_bytes: u32) callconv(.c) u32 {
    const k_jof_webp_bpp: u64 = 4;
    const k_jof_webp_scratch_mult: u64 = 2;
    const k_jof_webp_scratch_slack: u64 = 1 * 1024 * 1024;
    const max_dim = jof_h.k_ra8_webp_max_dim;
    if (max_width == 0 or max_width > max_dim or
        max_height == 0 or max_height > max_dim or
        max_src_bytes == 0)
    {
        return 0;
    }
    const frame: u64 = @as(u64, max_width) * @as(u64, max_height) * k_jof_webp_bpp;
    const scratch: u64 = k_jof_webp_scratch_mult * frame + k_jof_webp_scratch_slack;
    const total: u64 = @as(u64, max_src_bytes) + frame + scratch + @as(u64, jof_h.k_jof_carve_slack);
    if (total > std.math.maxInt(u32)) {
        return 0;
    }
    return @intCast(total);
}

const std = @import("std");

const bpp: u32 = 4; // k_jof_webp_bpp: WebP decodes to RGBA8888.

/// Band a decoded RGBA frame through the shared tile path, feeding
/// priv_jof_on_rows in tile_h slices so the WebP path emits byte-identical
/// output with the streaming decoders. Ports internal_webp_feed verbatim.
fn internal_webp_feed(
    st: *jof_h.jof_prod_state_t,
    frame: [*]const u8,
    w: c_ushort,
    h: c_ushort,
) callconv(.c) jof_h.ra8_err_t {
    const stride: u32 = @as(u32, w) * bpp;
    const th: u32 = @intCast(st.*.cfg.*.tile_h);
    var y: u32 = 0;
    while (y < @as(u32, h)) {
        const nrows: u32 = if ((@as(u32, h) - y) < th) @as(u32, h) - y else th;
        const err = jof_h.priv_jof_on_rows(
            st,
            frame + @as(usize, y) * @as(usize, stride),
            w,
            @intCast(y),
            @intCast(nrows),
            @intCast(bpp),
        );
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        y += nrows;
    }
    return jof_h.k_ra8_ok;
}

/// Pull the whole compressed WebP source into the webp_work arena, replaying
/// the sniffed prefix before the live source. Fails closed with
/// k_ra8_err_invalid_size when the arena fills without an end of stream.
/// Ports internal_webp_pull_all verbatim.
fn internal_webp_pull_all(
    pfx: *jof_h.jof_prefix_pull_t,
    dst: [*]u8,
    cap: usize,
    out_len: *usize,
) callconv(.c) jof_h.ra8_err_t {
    var len: usize = 0;
    while (len < cap) {
        var got: usize = 0;
        const err = jof_h.priv_jof_prefix_pull(pfx, @ptrCast(dst + len), cap - len, &got);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        if (got == 0) {
            out_len.* = len;
            return jof_h.k_ra8_ok;
        }
        len += got;
    }
    // The arena filled without hitting end of stream: the source is at least
    // as large as the whole arena, so there is no room for frame + scratch.
    return jof_h.k_ra8_err_invalid_size;
}

/// Whole-frame WebP transcode: pull the source, bind geometry, carve the
/// arena (source, aligned; frame; scratch for the libwebp decode), decode to
/// RGBA8888, and band the frame out through the shared tile path. Ports
/// priv_jof_webp_transcode verbatim.
pub export fn priv_jof_webp_transcode(
    st: *jof_h.jof_prod_state_t,
    pfx: *jof_h.jof_prefix_pull_t,
) callconv(.c) jof_h.ra8_err_t {
    const cfg: *const jof_h.jof_produce_cfg_t = @ptrCast(st.*.cfg);
    const arena_raw = cfg.webp_work;
    if (arena_raw == null) {
        return jof_h.k_ra8_err_not_supported; // WebP source but no whole-frame arena
    }
    const arena: [*]u8 = @ptrCast(arena_raw.?);
    const acap: usize = cfg.webp_work_cap;
    var src_len: usize = 0;
    var err = internal_webp_pull_all(pfx, arena, acap, &src_len);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    var w: c_uint = 0;
    var h: c_uint = 0;
    err = jof_h.ra8_webp_get_info(@ptrCast(arena), src_len, &w, &h);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    err = jof_h.priv_jof_on_geom(st, @intCast(w), @intCast(h), @intCast(bpp));
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    const mask: usize = @as(usize, @intCast(jof_h.k_jof_bump_align)) - 1;
    const used: usize = (src_len + mask) & ~mask;
    const frame64: u64 = @as(u64, w) * @as(u64, h) * @as(u64, bpp);
    if (used >= acap or frame64 >= @as(u64, acap - used)) {
        return jof_h.k_ra8_err_invalid_size; // no room for frame + at least some scratch
    }
    const frame_n: usize = @intCast(frame64);
    const frame: [*]u8 = arena + used;
    const scr_off: usize = used + frame_n;
    var wa = jof_h.ra8_webp_arena_t{
        .base = @ptrCast(arena + scr_off),
        .cap = acap - scr_off,
        .offset = 0,
        .live = 0,
        .high_water = 0,
    };
    err = jof_h.ra8_webp_decode_rgba(
        @ptrCast(arena),
        src_len,
        &wa,
        @ptrCast(frame),
        @as(usize, w) * @as(usize, bpp),
        frame_n,
        null,
        null,
    );
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    return internal_webp_feed(st, frame, @intCast(w), @intCast(h));
}

test "work bytes mirrors the C arithmetic" {
    try std.testing.expectEqual(@as(u32, 0), jof_webp_work_bytes(0, 100, 1000));
    try std.testing.expectEqual(@as(u32, 0), jof_webp_work_bytes(100, 0, 1000));
    try std.testing.expectEqual(@as(u32, 0), jof_webp_work_bytes(100, 100, 0));
    try std.testing.expectEqual(@as(u32, 0), jof_webp_work_bytes(@as(u16, @intCast(jof_h.k_ra8_webp_max_dim + 1)), 100, 1000));
    try std.testing.expectEqual(@as(u32, 0), jof_webp_work_bytes(100, @as(u16, @intCast(jof_h.k_ra8_webp_max_dim + 1)), 1000));
    // 100x100 frame: 40000 frame bytes, 84000 scratch, 1000 src, 128 slack.
    try std.testing.expectEqual(@as(u32, 1000 + 40000 + 80000 + 1048576 + 128), jof_webp_work_bytes(100, 100, 1000));
}
