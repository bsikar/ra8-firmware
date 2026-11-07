//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI `jof_produce.h`, `jof_internal.h` and the image-codec headers
//! publish, over the Zig implementation behind it.
//!
//! Every jof C TU still compiles from JOF_SOURCES; as functions port they are
//! deleted from the C and exported from here, and consumers link this archive
//! beside them (the ra8_fs pattern). The pure-math work sizer landed first
//! (RA8FW-1140); the streaming trio is the whole of jof_produce_webp.c
//! (RA8FW-1141), and the PNG chunk layer is the whole of jof_png_chunk.c
//! (RA8FW-1142). Both TUs are retired.

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

// ---------------------------------------------------------------------------
// PNG chunk layer. Ports jof_png_chunk.c whole (RA8FW-1142): the six
// RA8_PRIV entries are exported over the C ABI so jof_png.c keeps calling
// them through jof_png_internal.h, and the TU is retired. The IHDR, PLTE and
// tRNS helpers were file-local statics in the C and stay private here.

const crc: c_uint = @intCast(jof_h.k_ra8_png_crc_bytes);

/// Pull exactly `len` bytes; EOF mid-read is a protocol error.
/// Ports priv_jof_png_pull_exact verbatim.
pub export fn priv_jof_png_pull_exact(st: *jof_h.ra8_png_state_t, buf: [*c]u8, len: c_uint) callconv(.c) jof_h.ra8_err_t {
    var done: c_uint = 0;
    while (done < len) {
        var got: usize = 0;
        const err = st.*.pull.?(st.*.pull_ctx, buf + done, @as(usize, @intCast(len - done)), &got);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        if (got == 0) {
            return jof_h.k_ra8_err_protocol_error;
        }
        done += @intCast(got);
    }
    return jof_h.k_ra8_ok;
}

/// Discard exactly `len` source bytes over a fixed-size stack scratch.
/// Ports priv_jof_png_skip verbatim.
pub export fn priv_jof_png_skip(st: *jof_h.ra8_png_state_t, len: c_uint) callconv(.c) jof_h.ra8_err_t {
    var scratch: [jof_h.k_ra8_png_skip_chunk]u8 = @splat(0);
    var left: c_uint = len;
    while (left > 0) {
        const take: c_uint = if (left < jof_h.k_ra8_png_skip_chunk) left else jof_h.k_ra8_png_skip_chunk;
        const err = priv_jof_png_pull_exact(st, &scratch, take);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        left -= take;
    }
    return jof_h.k_ra8_ok;
}

/// Read one 8-byte chunk header (length + type, both big-endian) and enforce
/// the spec length cap. Ports priv_jof_png_chunk_hdr verbatim.
pub export fn priv_jof_png_chunk_hdr(st: *jof_h.ra8_png_state_t, out_len: [*c]c_uint, out_type: [*c]c_uint) callconv(.c) jof_h.ra8_err_t {
    var hdr: [jof_h.k_ra8_png_chunk_hdr]u8 = @splat(0);
    const err = priv_jof_png_pull_exact(st, &hdr, jof_h.k_ra8_png_chunk_hdr);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    const len: u32 = (@as(u32, hdr[0]) << 24) |
        (@as(u32, hdr[1]) << 16) |
        (@as(u32, hdr[2]) << 8) | @as(u32, hdr[3]);
    const typ: u32 = (@as(u32, hdr[4]) << 24) |
        (@as(u32, hdr[5]) << 16) |
        (@as(u32, hdr[6]) << 8) | @as(u32, hdr[7]);
    if (len > @as(u32, @intCast(jof_h.k_ra8_png_max_len))) {
        return jof_h.k_ra8_err_protocol_error;
    }
    out_len.* = len;
    out_type.* = typ;
    return jof_h.k_ra8_ok;
}

/// Map an IHDR colour type to its source bytes-per-pixel (bit depth fixed at
/// 8), or 0. Ports internal_png_src_channels verbatim.
fn internal_png_src_channels(color_type: u8) u8 {
    switch (color_type) {
        @intCast(jof_h.k_ra8_png_color_gray) => return @intCast(jof_h.k_ra8_png_ch_1),
        @intCast(jof_h.k_ra8_png_color_rgb) => return @intCast(jof_h.k_ra8_png_ch_3),
        @intCast(jof_h.k_ra8_png_color_pal) => return @intCast(jof_h.k_ra8_png_ch_1),
        @intCast(jof_h.k_ra8_png_color_ga) => return @intCast(jof_h.k_ra8_png_ch_2),
        @intCast(jof_h.k_ra8_png_color_rgba) => return @intCast(jof_h.k_ra8_png_ch_4),
        else => return 0,
    }
}

/// Validate the 13 IHDR payload bytes and bind the geometry fields, failing
/// closed on zero / over-cap dimensions, non-8-bit depth, unknown colour
/// types, non-zero compression / filter methods and interlacing.
/// Ports internal_png_check_ihdr verbatim.
fn internal_png_check_ihdr(st: *jof_h.ra8_png_state_t, ihdr: *const [jof_h.k_ra8_png_ihdr_len]u8, max_w: c_ushort, max_h: c_ushort) jof_h.ra8_err_t {
    const w32: u32 = (@as(u32, ihdr[0]) << 24) |
        (@as(u32, ihdr[1]) << 16) |
        (@as(u32, ihdr[2]) << 8) | @as(u32, ihdr[3]);
    const h32: u32 = (@as(u32, ihdr[4]) << 24) |
        (@as(u32, ihdr[5]) << 16) |
        (@as(u32, ihdr[6]) << 8) | @as(u32, ihdr[7]);
    if ((w32 == 0) or (w32 > @as(u32, max_w))) {
        return jof_h.k_ra8_err_invalid_size;
    }
    if ((h32 == 0) or (h32 > @as(u32, max_h))) {
        return jof_h.k_ra8_err_invalid_size;
    }
    if (ihdr[@intCast(jof_h.k_ra8_png_ihdr_ofs_depth)] != @as(u8, @intCast(jof_h.k_ra8_png_depth_8))) {
        return jof_h.k_ra8_err_not_supported;
    }
    st.*.src_ch = internal_png_src_channels(ihdr[@intCast(jof_h.k_ra8_png_ihdr_ofs_color)]);
    if (st.*.src_ch == 0) {
        return jof_h.k_ra8_err_not_supported;
    }
    if (ihdr[@intCast(jof_h.k_ra8_png_ihdr_ofs_compress)] != 0) {
        return jof_h.k_ra8_err_not_supported;
    }
    if (ihdr[@intCast(jof_h.k_ra8_png_ihdr_ofs_filter)] != 0) {
        return jof_h.k_ra8_err_not_supported;
    }
    if (ihdr[@intCast(jof_h.k_ra8_png_ihdr_ofs_interlace)] != 0) {
        return jof_h.k_ra8_err_not_supported; // Adam7 breaks streaming; fail closed
    }
    st.*.w = @intCast(w32);
    st.*.h = @intCast(h32);
    st.*.color_type = ihdr[@intCast(jof_h.k_ra8_png_ihdr_ofs_color)];
    return jof_h.k_ra8_ok;
}

/// Read the IHDR chunk (must be first), validate its exact 13-byte payload,
/// and skip the trailing CRC. Ports internal_png_parse_ihdr verbatim.
fn internal_png_parse_ihdr(st: *jof_h.ra8_png_state_t, max_w: c_ushort, max_h: c_ushort) jof_h.ra8_err_t {
    var len: c_uint = 0;
    var typ: c_uint = 0;
    var err = priv_jof_png_chunk_hdr(st, &len, &typ);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    if ((typ != @as(c_uint, @intCast(jof_h.k_ra8_png_type_ihdr))) or
        (len != @as(c_uint, @intCast(jof_h.k_ra8_png_ihdr_len))))
    {
        return jof_h.k_ra8_err_protocol_error;
    }
    var ihdr: [jof_h.k_ra8_png_ihdr_len]u8 = @splat(0);
    err = priv_jof_png_pull_exact(st, &ihdr, jof_h.k_ra8_png_ihdr_len);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    err = internal_png_check_ihdr(st, &ihdr, max_w, max_h);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    return priv_jof_png_skip(st, crc);
}

/// Parse a PLTE chunk into the palette table (colour type 3), validating the
/// payload shape and skipping the trailing CRC. Ports internal_png_parse_plte
/// verbatim.
fn internal_png_parse_plte(st: *jof_h.ra8_png_state_t, len: c_uint) jof_h.ra8_err_t {
    if ((len == 0) or ((len % jof_h.k_ra8_png_ch_3) != 0) or
        (len > @as(c_uint, @intCast(jof_h.k_ra8_png_plte_max))) or (st.*.has_plte != 0))
    {
        return jof_h.k_ra8_err_validation_failed;
    }
    const err = priv_jof_png_pull_exact(st, &st.*.palette, len);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    st.*.plte_count = @intCast(len / jof_h.k_ra8_png_ch_3);
    st.*.has_plte = 1;
    return priv_jof_png_skip(st, crc);
}

/// Parse a tRNS chunk (palette alpha; colour type 3 only); a tRNS on colour
/// types 0/2 is rejected k_ra8_err_not_supported rather than silently
/// flattened. Ports internal_png_parse_trns verbatim.
fn internal_png_parse_trns(st: *jof_h.ra8_png_state_t, len: c_uint) jof_h.ra8_err_t {
    if (st.*.color_type != @as(u8, @intCast(jof_h.k_ra8_png_color_pal))) {
        return jof_h.k_ra8_err_not_supported;
    }
    if ((len > @as(c_uint, @intCast(jof_h.k_ra8_png_trns_max))) or
        (st.*.has_plte == 0) or (st.*.has_trns != 0))
    {
        return jof_h.k_ra8_err_validation_failed;
    }
    const err = priv_jof_png_pull_exact(st, &st.*.trns, len);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    st.*.trns_count = @intCast(len);
    st.*.has_trns = 1;
    return priv_jof_png_skip(st, crc);
}

/// Verify the 8-byte signature, then parse + validate the IHDR.
/// Ports priv_jof_png_prologue verbatim.
pub export fn priv_jof_png_prologue(st: *jof_h.ra8_png_state_t, max_w: c_ushort, max_h: c_ushort) callconv(.c) jof_h.ra8_err_t {
    const png_sig = [jof_h.k_ra8_png_sig_bytes]u8{
        @intCast(jof_h.k_ra8_png_sig_high),
        'P',
        'N',
        'G',
        @intCast(jof_h.k_ra8_png_sig_cr),
        @intCast(jof_h.k_ra8_png_sig_lf),
        @intCast(jof_h.k_ra8_png_sig_sub),
        @intCast(jof_h.k_ra8_png_sig_lf),
    };
    var sig: [jof_h.k_ra8_png_sig_bytes]u8 = @splat(0);
    const err = priv_jof_png_pull_exact(st, &sig, jof_h.k_ra8_png_sig_bytes);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    if (!std.mem.eql(u8, &sig, &png_sig)) {
        return jof_h.k_ra8_err_protocol_error;
    }
    return internal_png_parse_ihdr(st, max_w, max_h);
}

/// Dispatch one pre-IDAT chunk (PLTE / tRNS / ancillary / stray IEND).
/// Ports priv_jof_png_pre_idat verbatim.
pub export fn priv_jof_png_pre_idat(st: *jof_h.ra8_png_state_t, len: c_uint, typ: c_uint) callconv(.c) jof_h.ra8_err_t {
    if (typ == @as(c_uint, @intCast(jof_h.k_ra8_png_type_plte))) {
        return internal_png_parse_plte(st, len);
    }
    if (typ == @as(c_uint, @intCast(jof_h.k_ra8_png_type_trns))) {
        return internal_png_parse_trns(st, len);
    }
    if (typ == @as(c_uint, @intCast(jof_h.k_ra8_png_type_iend))) {
        return jof_h.k_ra8_err_protocol_error; // IEND before any IDAT
    }
    return priv_jof_png_skip(st, len + crc);
}

/// Walk the post-IDAT chunks until IEND (ancillary chunks skipped); when the
/// zlib stream ended inside the last IDAT, consume that IDAT's trailing CRC
/// first. Ports priv_jof_png_finish verbatim.
pub export fn priv_jof_png_finish(st: *jof_h.ra8_png_state_t) callconv(.c) jof_h.ra8_err_t {
    var len: c_uint = st.*.pending_len;
    var typ: c_uint = st.*.pending_type;
    if (st.*.pending_valid == 0) {
        var err = priv_jof_png_skip(st, crc); // last IDAT's CRC
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        err = priv_jof_png_chunk_hdr(st, &len, &typ);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
    }
    var guard: c_uint = 0;
    while (guard < jof_h.k_ra8_png_max_chunks) : (guard += 1) {
        if (typ == @as(c_uint, @intCast(jof_h.k_ra8_png_type_iend))) {
            return jof_h.k_ra8_ok;
        }
        if (typ == @as(c_uint, @intCast(jof_h.k_ra8_png_type_idat))) {
            return jof_h.k_ra8_err_protocol_error; // IDAT after the stream completed
        }
        var err = priv_jof_png_skip(st, len + crc);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        err = priv_jof_png_chunk_hdr(st, &len, &typ);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
    }
    return jof_h.k_ra8_err_protocol_error;
}
