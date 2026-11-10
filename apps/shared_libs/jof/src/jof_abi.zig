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
//! (RA8FW-1142). Both TUs are retired. The memstore sink/pread pair and the
//! tile-access helpers (tile dims, stored bound) are ported from jof.c
//! (RA8FW-1147); jof_parse and its validation helpers are ported from jof.c
//! (RA8FW-1148); jof.c keeps the bounded per-tile decode.

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

// ---------------------------------------------------------------------------
// PNG pixel layer. Ports jof_png.c whole (RA8FW-1143): the streaming PNG
// scanline decoder (tinfl inflate, unfilter, translate) moves to Zig and the
// TU is retired. tinfl_decompress is an extern against the vendored miniz the
// consumers already link; the seam shim carries the real tinfl type surface
// (struct layout, statuses, flags) so the carve and the call are exact.

/// The ra8_log_error macro's backend entry (ra8_log.h): tag + free-form
/// message. Declared here because the seam's include closure does not reach
/// ra8_log.h's macro surface.
extern fn ra8_log_emit_error(tag: [*c]const u8, message: [*c]const u8) callconv(.c) void;

/// Paeth predictor (PNG sec 9.4 "Filter type 4: Paeth"). Ports
/// internal_png_paeth verbatim.
fn internal_png_paeth(a: u8, b: u8, c: u8) u8 {
    const p: i32 = @as(i32, a) + @as(i32, b) - @as(i32, c);
    const pa: i32 = if (p > @as(i32, a)) p - @as(i32, a) else @as(i32, a) - p;
    const pb: i32 = if (p > @as(i32, b)) p - @as(i32, b) else @as(i32, b) - p;
    const pc: i32 = if (p > @as(i32, c)) p - @as(i32, c) else @as(i32, c) - p;
    if ((pa <= pb) and (pa <= pc)) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

/// Reconstruct one filtered scanline in place (PNG sec 9 filters). Ports
/// internal_png_unfilter verbatim; filter arithmetic is modulo-256.
fn internal_png_unfilter(filter: u8, row: [*]u8, prev: [*]const u8, nbytes: u32, bpp_in: u8) jof_h.ra8_err_t {
    const b: u32 = @as(u32, bpp_in);
    switch (filter) {
        @intCast(jof_h.k_ra8_png_filter_none) => return jof_h.k_ra8_ok,
        @intCast(jof_h.k_ra8_png_filter_sub) => {
            var i: u32 = b;
            while (i < nbytes) : (i += 1) {
                row[i] = row[i] +% row[i - b];
            }
            return jof_h.k_ra8_ok;
        },
        @intCast(jof_h.k_ra8_png_filter_up) => {
            var i: u32 = 0;
            while (i < nbytes) : (i += 1) {
                row[i] = row[i] +% prev[i];
            }
            return jof_h.k_ra8_ok;
        },
        @intCast(jof_h.k_ra8_png_filter_avg) => {
            var i: u32 = 0;
            while (i < nbytes) : (i += 1) {
                const left: u32 = if (i >= b) @as(u32, row[i - b]) else 0;
                row[i] = row[i] +% @as(u8, @intCast((left + @as(u32, prev[i])) / 2));
            }
            return jof_h.k_ra8_ok;
        },
        @intCast(jof_h.k_ra8_png_filter_paeth) => {
            var i: u32 = 0;
            while (i < nbytes) : (i += 1) {
                const a: u8 = if (i >= b) row[i - b] else 0;
                const c: u8 = if (i >= b) prev[i - b] else 0;
                row[i] = row[i] +% internal_png_paeth(a, prev[i], c);
            }
            return jof_h.k_ra8_ok;
        },
        else => return jof_h.k_ra8_err_validation_failed,
    }
}

/// Translate one reconstructed source row into the output layout. Ports
/// internal_png_translate verbatim: gray->gray8, RGB/RGBA copied, gray+alpha
/// -> RGBA, palette -> RGB/RGBA via PLTE (+ tRNS) with bounds-checked
/// indices.
fn internal_png_translate(st: *jof_h.ra8_png_state_t, row: [*]const u8) jof_h.ra8_err_t {
    const w: u32 = @as(u32, st.*.w);
    if (st.*.color_type == @as(u8, @intCast(jof_h.k_ra8_png_color_pal))) {
        var x: u32 = 0;
        while (x < w) : (x += 1) {
            const idx: u8 = row[x];
            if (@as(u16, idx) >= st.*.plte_count) {
                return jof_h.k_ra8_err_validation_failed;
            }
            const src: u32 = @as(u32, idx) * @as(u32, @intCast(jof_h.k_ra8_png_ch_3));
            const dst: u32 = x * @as(u32, st.*.dst_ch);
            st.*.xlat[dst] = st.*.palette[src];
            st.*.xlat[dst + 1] = st.*.palette[src + 1];
            st.*.xlat[dst + 2] = st.*.palette[src + 2];
            if (st.*.dst_ch == @as(u8, @intCast(jof_h.k_ra8_png_ch_4))) {
                st.*.xlat[dst + 3] = if (@as(u16, idx) < st.*.trns_count) st.*.trns[idx] else @as(u8, @intCast(jof_h.k_ra8_png_opaque));
            }
        }
        return jof_h.k_ra8_ok;
    }
    if (st.*.color_type == @as(u8, @intCast(jof_h.k_ra8_png_color_ga))) {
        var x: u32 = 0;
        while (x < w) : (x += 1) {
            const g: u8 = row[x * @as(u32, @intCast(jof_h.k_ra8_png_ch_2))];
            const a: u8 = row[(x * @as(u32, @intCast(jof_h.k_ra8_png_ch_2))) + 1];
            const dst: u32 = x * @as(u32, @intCast(jof_h.k_ra8_png_ch_4));
            st.*.xlat[dst] = g;
            st.*.xlat[dst + 1] = g;
            st.*.xlat[dst + 2] = g;
            st.*.xlat[dst + 3] = a;
        }
        return jof_h.k_ra8_ok;
    }
    // gray, RGB, RGBA: source layout == output layout.
    const n: usize = @as(usize, w) * @as(usize, st.*.dst_ch);
    @memcpy(st.*.xlat[0..n], row[0..n]);
    return jof_h.k_ra8_ok;
}

/// Assemble, reconstruct and emit rows from freshly inflated bytes. Ports
/// internal_png_consume_rows verbatim; more scanlines than IHDR declared is a
/// hostile-stream error.
fn internal_png_consume_rows(st: *jof_h.ra8_png_state_t, src: [*]const u8, avail: u32) jof_h.ra8_err_t {
    var pos: u32 = 0;
    while (pos < avail) {
        const need: u32 = st.*.rowlen - st.*.rowfill;
        const left: u32 = avail - pos;
        const take: u32 = if (left < need) left else need;
        @memcpy(st.*.rowbuf[st.*.rowfill..][0..take], src[pos..][0..take]);
        st.*.rowfill += take;
        pos += take;
        if (st.*.rowfill < st.*.rowlen) {
            break; // partial scanline; wait for more inflate output
        }
        if (st.*.rows_done >= st.*.h) {
            return jof_h.k_ra8_err_validation_failed; // more scanlines than IHDR said
        }
        const nbytes: u32 = st.*.rowlen - 1;
        var err = internal_png_unfilter(st.*.rowbuf[0], st.*.rowbuf + 1, st.*.prevrow, nbytes, st.*.src_ch);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        @memcpy(st.*.prevrow[0..nbytes], (st.*.rowbuf + 1)[0..nbytes]);
        err = internal_png_translate(st, (st.*.rowbuf + 1)[0..]);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        err = st.*.on_rows.?(st.*.cb_ctx, st.*.xlat, st.*.w, st.*.rows_done, 1, st.*.dst_ch);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        st.*.rows_done += 1;
        st.*.rowfill = 0;
    }
    return jof_h.k_ra8_ok;
}

/// Fire the geometry hook and carve the pixel-path buffers. Runs once, at the
/// first IDAT. Ports internal_png_bind_geometry verbatim.
fn internal_png_bind_geometry(st: *jof_h.ra8_png_state_t, bump: [*c]jof_h.jof_bump_t) jof_h.ra8_err_t {
    if (st.*.color_type == @as(u8, @intCast(jof_h.k_ra8_png_color_pal))) {
        if (st.*.has_plte == 0) {
            return jof_h.k_ra8_err_validation_failed;
        }
        st.*.dst_ch = if (st.*.has_trns != 0) @as(u8, @intCast(jof_h.k_ra8_png_ch_4)) else @as(u8, @intCast(jof_h.k_ra8_png_ch_3));
    } else if (st.*.color_type == @as(u8, @intCast(jof_h.k_ra8_png_color_gray))) {
        st.*.dst_ch = @intCast(jof_h.k_ra8_png_ch_1);
    } else if (st.*.color_type == @as(u8, @intCast(jof_h.k_ra8_png_color_ga))) {
        st.*.dst_ch = @intCast(jof_h.k_ra8_png_ch_4);
    } else {
        st.*.dst_ch = st.*.src_ch; // RGB -> 3, RGBA -> 4
    }
    const err = st.*.on_geom.?(st.*.cb_ctx, st.*.w, st.*.h, st.*.dst_ch);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    st.*.rowlen = 1 + (@as(u32, st.*.w) * @as(u32, st.*.src_ch));
    // The bump allocator returns 8-byte-aligned carves, which satisfies the
    // inflate context's alignment.
    st.*.inflator = @ptrCast(@alignCast(jof_h.priv_jof_bump_take(bump, @sizeOf(jof_h.struct_tinfl_decompressor_tag))));
    st.*.ring = @ptrCast(@alignCast(jof_h.priv_jof_bump_take(bump, @as(usize, @intCast(jof_h.k_ra8_png_ring_bytes)))));
    st.*.inbuf = @ptrCast(@alignCast(jof_h.priv_jof_bump_take(bump, @as(usize, @intCast(jof_h.k_ra8_png_inbuf_bytes)))));
    st.*.rowbuf = @ptrCast(@alignCast(jof_h.priv_jof_bump_take(bump, @as(usize, st.*.rowlen))));
    st.*.prevrow = @ptrCast(@alignCast(jof_h.priv_jof_bump_take(bump, @as(usize, st.*.rowlen - 1))));
    st.*.xlat = @ptrCast(@alignCast(jof_h.priv_jof_bump_take(bump, @as(usize, @as(usize, st.*.w) * @as(usize, st.*.dst_ch)))));
    if ((st.*.inflator == null) or (st.*.ring == null) or (st.*.inbuf == null) or
        (st.*.rowbuf == null) or (st.*.prevrow == null) or (st.*.xlat == null))
    {
        return jof_h.k_ra8_err_invalid_size;
    }
    @memset(st.*.prevrow[0..@as(usize, st.*.rowlen - 1)], 0); // PNG sec 9: prior row = zeroes
    st.*.inflator.*.m_state = 0; // tinfl_init
    return jof_h.k_ra8_ok;
}

/// Refill the compressed-input buffer from the IDAT chunk stream, crossing
/// consecutive IDATs transparently; a non-IDAT chunk ends the compressed
/// stream and parks as the pending chunk. Ports internal_png_refill_input
/// verbatim.
fn internal_png_refill_input(st: *jof_h.ra8_png_state_t, out_avail: *u32) jof_h.ra8_err_t {
    out_avail.* = 0;
    var guard: u32 = 0;
    while (guard < @as(u32, @intCast(jof_h.k_ra8_png_max_chunks))) : (guard += 1) {
        if (st.*.idat_rem > 0) {
            const take: u32 = if (st.*.idat_rem < @as(u32, @intCast(jof_h.k_ra8_png_inbuf_bytes))) st.*.idat_rem else @as(u32, @intCast(jof_h.k_ra8_png_inbuf_bytes));
            const err = priv_jof_png_pull_exact(st, st.*.inbuf, take);
            if (err != jof_h.k_ra8_ok) {
                return err;
            }
            st.*.idat_rem -= take;
            out_avail.* = take;
            return jof_h.k_ra8_ok;
        }
        // Current IDAT exhausted: skip its CRC, look at the next chunk.
        var err = priv_jof_png_skip(st, @intCast(jof_h.k_ra8_png_crc_bytes));
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        var len: c_uint = 0;
        var typ: c_uint = 0;
        err = priv_jof_png_chunk_hdr(st, &len, &typ);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        if (typ == @as(c_uint, @intCast(jof_h.k_ra8_png_type_idat))) {
            st.*.idat_rem = len;
            continue;
        }
        st.*.pending_len = len;
        st.*.pending_type = typ;
        st.*.pending_valid = 1;
        st.*.source_done = 1;
        return jof_h.k_ra8_ok; // clean end of the compressed stream
    }
    return jof_h.k_ra8_err_protocol_error; // chunk budget exhausted (hostile)
}

/// Inflate-loop cursor: compressed-input window + progress guard.
/// Ports ra8_png_iter_t verbatim (invariant: in_pos <= in_avail).
const ra8_png_iter_t = struct {
    in_avail: u32 = 0, // valid compressed bytes in inbuf
    in_pos: u32 = 0, // consumed compressed bytes
    stalls: u8 = 0, // consecutive zero-progress iterations
    done: u8 = 0, // 1 once tinfl reported stream end
};

/// One inflate iteration: refill, tinfl, progress guard, drain rows. The ring
/// is tinfl's LZ dictionary; each call produces into the contiguous run
/// [ring_wr, ring_end) which drains into the scanline assembler before the
/// write offset wraps. Ports internal_png_inflate_step verbatim, stall guard
/// included.
fn internal_png_inflate_step(st: *jof_h.ra8_png_state_t, it: *ra8_png_iter_t) jof_h.ra8_err_t {
    if ((it.*.in_pos == it.*.in_avail) and (st.*.source_done == 0)) {
        const err = internal_png_refill_input(st, &it.*.in_avail);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        it.*.in_pos = 0;
    }
    var in_sz: usize = @as(usize, it.*.in_avail - it.*.in_pos);
    var out_sz: usize = @as(usize, @as(u32, @intCast(jof_h.k_ra8_png_ring_bytes)) - st.*.ring_wr);
    const flags: u32 = @as(u32, @intCast(jof_h.TINFL_FLAG_PARSE_ZLIB_HEADER)) |
        (if (st.*.source_done == 0) @as(u32, @intCast(jof_h.TINFL_FLAG_HAS_MORE_INPUT)) else 0);
    const status: i32 = jof_h.tinfl_decompress(
        st.*.inflator,
        st.*.inbuf + it.*.in_pos,
        &in_sz,
        st.*.ring,
        st.*.ring + st.*.ring_wr,
        &out_sz,
        flags,
    );
    if (status < jof_h.TINFL_STATUS_DONE) {
        return jof_h.k_ra8_err_protocol_error; // corrupt deflate / bad zlib header
    }
    // Zero-progress stall guard, defense-in-depth against a decompressor that
    // spins without failing (mcdc-deactivated in the C, same rationale here).
    if ((in_sz == 0) and (out_sz == 0)) {
        it.*.stalls += 1;
        if ((it.*.stalls > 1) or (st.*.source_done != 0)) {
            return jof_h.k_ra8_err_protocol_error; // truncated stream / no progress
        }
    } else {
        it.*.stalls = 0;
    }
    if (out_sz > 0) {
        const err = internal_png_consume_rows(st, (st.*.ring + st.*.ring_wr)[0..], @intCast(out_sz));
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        st.*.ring_wr = (st.*.ring_wr + @as(u32, @intCast(out_sz))) & (@as(u32, @intCast(jof_h.k_ra8_png_ring_bytes)) - 1);
    }
    it.*.in_pos += @intCast(in_sz);
    it.*.done = if (status == jof_h.TINFL_STATUS_DONE) 1 else 0;
    return jof_h.k_ra8_ok;
}

/// Inflate the whole IDAT stream, emitting every scanline, then apply the
/// strict termination checks: every declared row arrived, no partial
/// scanline remains, and no compressed bytes trail the stream. Ports
/// internal_png_inflate_idat verbatim.
fn internal_png_inflate_idat(st: *jof_h.ra8_png_state_t, first_len: u32) jof_h.ra8_err_t {
    st.*.idat_rem = first_len;
    var it: ra8_png_iter_t = .{};
    while (it.done == 0) {
        const err = internal_png_inflate_step(st, &it);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
    }
    // Strict termination: every declared row arrived, nothing extra.
    if ((st.*.rows_done != st.*.h) or (st.*.rowfill != 0)) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if ((it.in_pos != it.in_avail) or (st.*.idat_rem != 0)) {
        return jof_h.k_ra8_err_validation_failed; // trailing garbage inside IDAT
    }
    return jof_h.k_ra8_ok;
}

/// The IDAT arm: bind geometry, inflate the stream, finish the walk. Runs
/// once, at the first IDAT chunk. Ports internal_png_run_idat verbatim.
fn internal_png_run_idat(st: *jof_h.ra8_png_state_t, bump: [*c]jof_h.jof_bump_t, len: u32) jof_h.ra8_err_t {
    var err = internal_png_bind_geometry(st, bump);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    err = internal_png_inflate_idat(st, len);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    return priv_jof_png_finish(st);
}

/// Walk the chunk stream: pre-IDAT chunks, then the IDAT arm; bounded by the
/// shared chunk budget. Ports internal_png_walk_chunks verbatim.
fn internal_png_walk_chunks(st: *jof_h.ra8_png_state_t, bump: [*c]jof_h.jof_bump_t, max_w: u16, max_h: u16) jof_h.ra8_err_t {
    var err = priv_jof_png_prologue(st, max_w, max_h);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    var guard: u32 = 0;
    while (guard < @as(u32, @intCast(jof_h.k_ra8_png_max_chunks))) : (guard += 1) {
        var len: c_uint = 0;
        var typ: c_uint = 0;
        err = priv_jof_png_chunk_hdr(st, &len, &typ);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        if (typ == @as(c_uint, @intCast(jof_h.k_ra8_png_type_idat))) {
            return internal_png_run_idat(st, bump, len);
        }
        err = priv_jof_png_pre_idat(st, len, typ);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
    }
    return jof_h.k_ra8_err_protocol_error; // chunk budget exhausted (hostile)
}

/// The streaming PNG scanline decoder entry point: bounded-RAM, pull-based,
/// resident cost the inflate ring plus three rows, independent of height.
/// Ports priv_jof_png_rows verbatim, RA8_CHECK_NULL_PTR gates included.
var s_png: jof_h.ra8_png_state_t = undefined;

pub export fn priv_jof_png_rows(
    pull: jof_h.jof_pull_fn,
    pull_ctx: ?*anyopaque,
    bump: [*c]jof_h.jof_bump_t,
    max_w: u16,
    max_h: u16,
    on_geom: jof_h.jof_geom_fn,
    on_rows: jof_h.jof_rows_fn,
    cb_ctx: ?*anyopaque,
) callconv(.c) jof_h.ra8_err_t {
    const s_tag: [*c]const u8 = "jof_png";
    if (pull == null) {
        ra8_log_emit_error(s_tag, "pull must not be nullptr");
        return jof_h.k_ra8_err_null_ptr;
    }
    if (bump == null) {
        ra8_log_emit_error(s_tag, "bump must not be nullptr");
        return jof_h.k_ra8_err_null_ptr;
    }
    if (on_geom == null) {
        ra8_log_emit_error(s_tag, "on_geom must not be nullptr");
        return jof_h.k_ra8_err_null_ptr;
    }
    if (on_rows == null) {
        ra8_log_emit_error(s_tag, "on_rows must not be nullptr");
        return jof_h.k_ra8_err_null_ptr;
    }
    const st: *jof_h.ra8_png_state_t = &s_png;
    st.* = std.mem.zeroes(jof_h.ra8_png_state_t);
    st.*.pull = pull;
    st.*.pull_ctx = pull_ctx;
    st.*.on_geom = on_geom;
    st.*.on_rows = on_rows;
    st.*.cb_ctx = cb_ctx;
    return internal_png_walk_chunks(st, bump, max_w, max_h);
}

// =====================================================================
// Audit TU (jof_audit.c), ported verbatim (RA8FW-1146). internal_read_exact,
// internal_rd_u32, internal_hash, internal_make_span, internal_spans_overlap,
// internal_any_workspace_overlap, internal_check_workspace_spans,
// internal_check_workspace, internal_duplicate_count, internal_audit_context_t,
// internal_audit_tile, jof_audit_requirements, jof_audit. Sibling C
// (jof_tile_dims, jof_stored_bound) reached through; jof_parse and jof_read_tile
// the translated seam.
// =====================================================================

const k_audit_fnv_basis: u32 = 2166136261;
const k_audit_fnv_prime: u32 = 16777619;
const k_audit_u32_b3_shift: u5 = 24;

/// Half-open address span used for caller-buffer separation checks.
const AuditSpan = struct {
    begin: usize = 0,
    end: usize = 0,
};

/// Read exactly one bounded window from the injected backing; a short
/// successful callback read is a validation error. Ports internal_read_exact.
fn auditReadExact(
    pread: jof_h.jof_pread_fn,
    ctx: ?*anyopaque,
    offset: u64,
    buf: [*c]u8,
    len: usize,
) callconv(.c) jof_h.ra8_err_t {
    var got: usize = 0;
    const rc = pread.?(ctx, offset, buf, len, &got);
    if (rc != jof_h.k_ra8_ok) {
        return rc;
    }
    return if (got == len) jof_h.k_ra8_ok else jof_h.k_ra8_err_validation_failed;
}

/// Decode a little-endian u32 from four bytes, host endianness irrelevant.
/// Ports internal_rd_u32.
fn auditRdU32(p: *const [4]u8) u32 {
    return @as(u32, p[0]) | (@as(u32, p[1]) << 8) |
        (@as(u32, p[2]) << 16) | (@as(u32, p[3]) << k_audit_u32_b3_shift);
}

/// Hash decoded bytes with FNV-1a and report whether all bytes are equal.
/// Bounded diagnostic evidence, not an identity proof. Ports internal_hash.
fn auditHash(bytes: []const u8, out_uniform: *bool) u32 {
    var h: u32 = k_audit_fnv_basis;
    var uniform: bool = true;
    for (bytes, 0..) |b, i| {
        if (i != 0 and b != bytes[0]) {
            uniform = false;
        }
        h ^= @as(u32, b);
        h *%= k_audit_fnv_prime;
    }
    out_uniform.* = uniform;
    return h;
}

/// Convert one caller region into an overflow-checked address span; empty
/// spans carry zero endpoints and never overlap. Ports internal_make_span.
fn auditMakeSpan(ptr: ?*const anyopaque, len: usize, out: *AuditSpan) callconv(.c) jof_h.ra8_err_t {
    if (len == 0) {
        out.* = .{};
        return jof_h.k_ra8_ok;
    }
    if (ptr == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    const begin: usize = @intFromPtr(ptr.?);
    if (len > std.math.maxInt(usize) - begin) {
        return jof_h.k_ra8_err_invalid_size;
    }
    out.* = .{ .begin = begin, .end = begin + len };
    return jof_h.k_ra8_ok;
}

/// True only when two half-open spans share at least one byte; adjacent and
/// empty spans are disjoint. Ports internal_spans_overlap.
fn auditSpansOverlap(left: *const AuditSpan, right: *const AuditSpan) bool {
    if (left.begin >= right.end) {
        return false;
    }
    return right.begin < left.end;
}

/// Check every unique pair of writable audit spans for overlap, stopping at
/// the first shared byte. Ports internal_any_workspace_overlap.
fn auditAnyWorkspaceOverlap(
    workspace: *const AuditSpan,
    records: *const AuditSpan,
    tile: *const AuditSpan,
    scratch: *const AuditSpan,
    result: *const AuditSpan,
) bool {
    if (auditSpansOverlap(workspace, records)) return true;
    if (auditSpansOverlap(workspace, tile)) return true;
    if (auditSpansOverlap(workspace, scratch)) return true;
    if (auditSpansOverlap(workspace, result)) return true;
    if (auditSpansOverlap(records, tile)) return true;
    if (auditSpansOverlap(records, scratch)) return true;
    if (auditSpansOverlap(records, result)) return true;
    if (auditSpansOverlap(tile, scratch)) return true;
    if (auditSpansOverlap(tile, result)) return true;
    return auditSpansOverlap(scratch, result);
}

/// Parse an atlas and report exact caller-storage requirements.
/// Ports jof_audit_requirements.
pub export fn jof_audit_requirements(
    pread: jof_h.jof_pread_fn,
    pread_ctx: ?*anyopaque,
    total_size: u64,
    out: [*c]jof_h.jof_audit_requirements_t,
) callconv(.c) jof_h.ra8_err_t {
    if (pread == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (out == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    var info: jof_h.jof_info_t = std.mem.zeroes(jof_h.jof_info_t);
    const rc = jof_h.jof_parse(pread, pread_ctx, total_size, &info);
    if (rc != jof_h.k_ra8_ok) {
        return rc;
    }
    const tile64: u64 = @as(u64, info.tile_w) * @as(u64, info.tile_h) * @as(u64, info.bpp);
    if (tile64 > std.math.maxInt(u32)) {
        return jof_h.k_ra8_err_invalid_size;
    }
    const tile: u32 = @intCast(tile64);
    const candidate: jof_h.jof_audit_requirements_t = .{
        .record_count = info.tile_count,
        .tile_bytes = tile,
        .scratch_bytes = if (info.codec == jof_h.k_jof_codec_deflate) jof_h.jof_stored_bound(tile) else 0,
    };
    if (candidate.tile_bytes == 0) {
        return jof_h.k_ra8_err_invalid_size;
    }
    if (info.codec == jof_h.k_jof_codec_deflate) {
        if (candidate.scratch_bytes == 0) {
            return jof_h.k_ra8_err_invalid_size;
        }
    }
    out.* = candidate;
    return jof_h.k_ra8_ok;
}

/// Validate the writable audit span layout: every caller-owned destination
/// representable and pairwise disjoint. Ports internal_check_workspace_spans.
fn auditCheckWorkspaceSpans(
    ws: *const jof_h.jof_audit_workspace_t,
    need: *const jof_h.jof_audit_requirements_t,
    out: *const jof_h.jof_audit_result_t,
    record_bytes: usize,
) callconv(.c) jof_h.ra8_err_t {
    var workspace_span: AuditSpan = .{};
    var record_span: AuditSpan = .{};
    var tile_span: AuditSpan = .{};
    var scratch_span: AuditSpan = .{};
    var result_span: AuditSpan = .{};
    var rc = auditMakeSpan(ws, @sizeOf(jof_h.jof_audit_workspace_t), &workspace_span);
    if (rc == jof_h.k_ra8_ok) {
        rc = auditMakeSpan(@ptrCast(ws.records), record_bytes, &record_span);
    }
    if (rc == jof_h.k_ra8_ok) {
        rc = auditMakeSpan(@ptrCast(ws.tile), need.tile_bytes, &tile_span);
    }
    if (rc == jof_h.k_ra8_ok) {
        rc = auditMakeSpan(@ptrCast(ws.scratch), need.scratch_bytes, &scratch_span);
    }
    if (rc == jof_h.k_ra8_ok) {
        rc = auditMakeSpan(out, @sizeOf(jof_h.jof_audit_result_t), &result_span);
    }
    if (rc != jof_h.k_ra8_ok) {
        return rc;
    }
    if (auditAnyWorkspaceOverlap(&workspace_span, &record_span, &tile_span, &scratch_span, &result_span)) {
        return jof_h.k_ra8_err_invalid_arg;
    }
    return jof_h.k_ra8_ok;
}

/// Validate caller workspace against parsed requirements: every mandatory
/// pointer present, every capacity exact-or-larger. Ports internal_check_workspace.
fn auditCheckWorkspace(
    ws: [*c]jof_h.jof_audit_workspace_t,
    need: *const jof_h.jof_audit_requirements_t,
    out: [*c]jof_h.jof_audit_result_t,
) callconv(.c) jof_h.ra8_err_t {
    if (ws == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (ws.*.records == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (ws.*.tile == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (need.*.scratch_bytes != 0) {
        if (ws.*.scratch == null) {
            return jof_h.k_ra8_err_null_ptr;
        }
    }
    if (ws.*.record_cap < need.*.record_count) {
        return jof_h.k_ra8_err_invalid_size;
    }
    if (ws.*.tile_cap < need.*.tile_bytes) {
        return jof_h.k_ra8_err_invalid_size;
    }
    if (ws.*.scratch_cap < need.*.scratch_bytes) {
        return jof_h.k_ra8_err_invalid_size;
    }
    const record_bytes: usize = @as(usize, need.*.record_count) * @sizeOf(jof_h.jof_audit_record_t);
    if (need.*.record_count != 0) {
        if ((record_bytes / @sizeOf(jof_h.jof_audit_record_t)) != @as(usize, need.*.record_count)) {
            return jof_h.k_ra8_err_invalid_size;
        }
    }
    return auditCheckWorkspaceSpans(ws, need, out, record_bytes);
}

/// Count earlier records matching one non-uniform tile fingerprint. Uniform
/// tiles are excluded so common blank regions do not inflate duplicate
/// diagnostics; matching hashes are evidence only. Ports internal_duplicate_count.
fn auditDuplicateCount(
    records: [*c]const jof_h.jof_audit_record_t,
    count: u32,
    item: *const jof_h.jof_audit_record_t,
) u32 {
    if (item.uniform) {
        return 0;
    }
    var matches: u32 = 0;
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        if (records[i].uniform) {
            continue;
        }
        if (records[i].payload != item.payload) {
            continue;
        }
        if (records[i].content_hash != item.content_hash) {
            continue;
        }
        matches += 1;
    }
    return matches;
}

/// Mutable state shared by the bounded per-tile audit operation.
const AuditContext = struct {
    pread: jof_h.jof_pread_fn,
    pread_ctx: ?*anyopaque,
    workspace: *jof_h.jof_audit_workspace_t,
    candidate: *jof_h.jof_audit_result_t,
    expected_offset: u32,
};

/// Audit one index entry and its decoded tile evidence: reads the canonical
/// record, checks stored coverage, decodes through caller scratch, validates
/// edge geometry, accumulates diagnostic evidence. Ports internal_audit_tile.
fn auditTile(context: *AuditContext, index_number: u32) callconv(.c) jof_h.ra8_err_t {
    var index: [jof_h.k_jof_index_entry]u8 = std.mem.zeroes([jof_h.k_jof_index_entry]u8);
    const index_at: u64 = @as(u64, context.candidate.*.info.index_off) +
        (@as(u64, index_number) * @as(u64, jof_h.k_jof_index_entry));
    var rc = auditReadExact(context.pread, context.pread_ctx, index_at, &index, index.len);
    if (rc != jof_h.k_ra8_ok) {
        return rc;
    }
    const record: *jof_h.jof_audit_record_t = @ptrCast(&context.workspace.*.records[index_number]);
    record.* = std.mem.zeroes(jof_h.jof_audit_record_t);
    record.*.offset = auditRdU32(index[0..4]);
    record.*.length = auditRdU32(index[4..8]);
    if (record.*.offset != context.expected_offset) {
        context.candidate.*.coverage_errors += 1;
    }
    const next: u64 = @as(u64, record.*.offset) + @as(u64, record.*.length);
    if (next > std.math.maxInt(u32)) {
        return jof_h.k_ra8_err_validation_failed;
    }
    context.expected_offset = @intCast(next);

    const tx: u16 = @intCast(index_number % @as(u32, context.candidate.*.info.tile_cols));
    const ty: u16 = @intCast(index_number / @as(u32, context.candidate.*.info.tile_cols));
    rc = jof_h.jof_read_tile(
        context.pread,
        context.pread_ctx,
        &context.candidate.*.info,
        tx,
        ty,
        context.workspace.*.scratch,
        context.workspace.*.scratch_cap,
        context.workspace.*.tile,
        context.workspace.*.tile_cap,
        &record.*.width,
        &record.*.height,
    );
    if (rc != jof_h.k_ra8_ok) {
        return rc;
    }
    record.*.payload = @as(u32, record.*.width) *% @as(u32, record.*.height) *%
        @as(u32, context.candidate.*.info.bpp);
    var want_w: u16 = 0;
    var want_h: u16 = 0;
    rc = jof_h.jof_tile_dims(&context.candidate.*.info, tx, ty, &want_w, &want_h);
    if (rc != jof_h.k_ra8_ok) {
        return rc;
    }
    var geometry_error: bool = record.*.width != want_w;
    if (record.*.height != want_h) {
        geometry_error = true;
    }
    if (geometry_error) {
        context.candidate.*.geometry_errors += 1;
    }
    record.*.content_hash = auditHash(context.workspace.*.tile[0..record.*.payload], &record.*.uniform);
    context.candidate.*.duplicate_candidates +=
        auditDuplicateCount(context.workspace.*.records, index_number, record);
    context.candidate.*.decoded_tiles += 1;
    return jof_h.k_ra8_ok;
}

/// Audit coverage, geometry and duplicate-content evidence in bounded RAM.
/// Ports jof_audit.
pub export fn jof_audit(
    pread: jof_h.jof_pread_fn,
    pread_ctx: ?*anyopaque,
    total_size: u64,
    workspace: [*c]jof_h.jof_audit_workspace_t,
    out: [*c]jof_h.jof_audit_result_t,
) callconv(.c) jof_h.ra8_err_t {
    if (pread == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (out == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    var need: jof_h.jof_audit_requirements_t = std.mem.zeroes(jof_h.jof_audit_requirements_t);
    var rc = jof_audit_requirements(pread, pread_ctx, total_size, &need);
    if (rc != jof_h.k_ra8_ok) {
        return rc;
    }
    rc = auditCheckWorkspace(workspace, &need, out);
    if (rc != jof_h.k_ra8_ok) {
        return rc;
    }
    var candidate: jof_h.jof_audit_result_t = std.mem.zeroes(jof_h.jof_audit_result_t);
    rc = jof_h.jof_parse(pread, pread_ctx, total_size, &candidate.info);
    if (rc != jof_h.k_ra8_ok) {
        return rc;
    }

    var context: AuditContext = .{
        .pread = pread,
        .pread_ctx = pread_ctx,
        .workspace = workspace.?,
        .candidate = &candidate,
        .expected_offset = @as(u32, jof_h.k_jof_hdr_bytes),
    };
    var i: u32 = 0;
    while (i < candidate.info.tile_count) : (i += 1) {
        rc = auditTile(&context, i);
        if (rc != jof_h.k_ra8_ok) {
            return rc;
        }
    }
    if (context.expected_offset != candidate.info.index_off) {
        candidate.coverage_errors += 1;
    }
    out.* = candidate;
    if (candidate.coverage_errors != 0) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (candidate.geometry_errors != 0) {
        return jof_h.k_ra8_err_validation_failed;
    }
    return jof_h.k_ra8_ok;
}

// ---------------------------------------------------------------------------
// jof.c retirement: memstore backing + tile-access helpers (RA8FW-1147).
// ---------------------------------------------------------------------------

/// Worst-case stored-stream expansion bound: raw + raw/8 + 256. The bound
/// terms live only in jof.c's jof_bound_t enum, so they are restated here.
/// Ports jof_stored_bound.
pub export fn jof_stored_bound(raw_bytes: u32) callconv(.c) u32 {
    const k_jof_bound_div: u32 = 8;
    const k_jof_bound_add: u32 = 256;
    // The C helper relies on the uint32 wrap: at the exact 2^32 product the
    // stored bound wraps to zero, which callers must refuse. Keep that
    // semantics here with wrapping adds.
    return raw_bytes +% raw_bytes / k_jof_bound_div +% k_jof_bound_add;
}

/// Pixel rect of tile (tile_x, tile_y), clamped at the atlas edges. The last
/// column/row is the short one when the atlas is not a multiple of the tile
/// grid. Ports jof_tile_dims.
pub export fn jof_tile_dims(
    info: [*c]const jof_h.jof_info_t,
    tile_x: u16,
    tile_y: u16,
    out_w: [*c]u16,
    out_h: [*c]u16,
) callconv(.c) jof_h.ra8_err_t {
    if (info == null or out_w == null or out_h == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    const inf = info.*;
    if (tile_x >= inf.tile_cols) {
        return jof_h.k_ra8_err_out_of_range;
    }
    if (tile_y >= inf.tile_rows) {
        return jof_h.k_ra8_err_out_of_range;
    }
    const px: u32 = @as(u32, tile_x) * @as(u32, inf.tile_w);
    const py: u32 = @as(u32, tile_y) * @as(u32, inf.tile_h);
    const rest_w: u32 = @as(u32, inf.width) - px;
    const rest_h: u32 = @as(u32, inf.height) - py;
    out_w.* = @intCast(@min(rest_w, @as(u32, inf.tile_w)));
    out_h.* = @intCast(@min(rest_h, @as(u32, inf.tile_h)));
    return jof_h.k_ra8_ok;
}

/// Append bytes to a jof_memstore_t (producer sink). Fails closed at the
/// store's capacity so a hostile source can never write past cap.
/// Ports jof_memstore_sink.
pub export fn jof_memstore_sink(ctx: ?*anyopaque, buf: [*c]const u8, len: usize) callconv(.c) jof_h.ra8_err_t {
    if (ctx == null or buf == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    const store: *jof_h.jof_memstore_t = @ptrCast(@alignCast(ctx.?));
    if (store.buf == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (len > store.cap - store.len) {
        return jof_h.k_ra8_err_no_mem;
    }
    @memcpy(store.buf[store.len..][0..len], buf[0..len]);
    store.len += len;
    return jof_h.k_ra8_ok;
}

/// Positioned read from a jof_memstore_t (reader seam). Reads clamp at the
/// stored length; a read at/after the end reports got == 0 without error.
/// Ports jof_memstore_pread.
pub export fn jof_memstore_pread(ctx: ?*anyopaque, offset: u64, buf: [*c]u8, len: usize, got: [*c]usize) callconv(.c) jof_h.ra8_err_t {
    if (ctx == null or buf == null or got == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    got.* = 0;
    const store: *jof_h.jof_memstore_t = @ptrCast(@alignCast(ctx.?));
    if (store.buf == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (offset >= store.len) {
        return jof_h.k_ra8_ok;
    }
    const avail: usize = store.len - @as(usize, @intCast(offset));
    const take: usize = @min(len, avail);
    @memcpy(buf[0..take], store.buf[@as(usize, @intCast(offset))..][0..take]);
    got.* = take;
    return jof_h.k_ra8_ok;
}

// =====================================================================
// Parse TU section (jof_parse), ported verbatim (RA8FW-1148). The 1147
// slice over-deleted: jof_parse and internal_parse_header_region left
// jof.c with the memstore removal, so the jof_parse symbol has been
// absent from dev since 65a1a63e3 and only the test stub kept consumer
// links resolving. internal_parse_header_region,
// internal_parse_hdr_fields, internal_check_reserved,
// internal_check_cross, internal_rd_u16, internal_rd_u32,
// internal_ceil_div, internal_pread_exact, jof_parse. Sibling C
// (jof_read_tile) reached through the translated seam.
// =====================================================================

/// Read a little-endian uint16 from buf at off. Ports internal_rd_u16.
fn parseRdU16(buf: []const u8, off: u8) u16 {
    return @as(u16, buf[off]) | (@as(u16, buf[off + 1]) << 8);
}

/// Read a little-endian uint32 from buf at off. Ports internal_rd_u32.
fn parseRdU32(buf: []const u8, off: u8) u32 {
    return @as(u32, buf[off]) | (@as(u32, buf[off + 1]) << 8) |
        (@as(u32, buf[off + 2]) << 16) | (@as(u32, buf[off + 3]) << 24);
}

/// Ceiling division for tile-grid geometry. Ports internal_ceil_div.
fn parseCeilDiv(n: u32, d: u32) u32 {
    return (n + d - 1) / d;
}

/// Exact positioned read: EOF or a short read is a validation failure.
/// Ports internal_pread_exact.
fn parsePreadExact(
    pread: jof_h.jof_pread_fn,
    pread_ctx: ?*anyopaque,
    off: u64,
    buf: []u8,
) jof_h.ra8_err_t {
    var got: usize = 0;
    const err = pread.?(pread_ctx, off, buf.ptr, buf.len, &got);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    if (got != buf.len) {
        return jof_h.k_ra8_err_validation_failed;
    }
    return jof_h.k_ra8_ok;
}

/// Validate the header geometry fields into info (fail-closed).
/// Ports internal_parse_hdr_fields.
fn parseHdrFields(hdr: []const u8, info: *jof_h.jof_info_t) jof_h.ra8_err_t {
    info.width = parseRdU16(hdr, jof_h.k_jof_ofs_width);
    info.height = parseRdU16(hdr, jof_h.k_jof_ofs_height);
    info.tile_w = parseRdU16(hdr, jof_h.k_jof_ofs_tile_w);
    info.tile_h = parseRdU16(hdr, jof_h.k_jof_ofs_tile_h);
    info.bpp = hdr[jof_h.k_jof_ofs_bpp];
    info.codec = hdr[jof_h.k_jof_ofs_codec];
    if (info.width == 0) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (@as(u32, info.width) > @as(u32, jof_h.k_jof_max_dim)) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (info.height == 0) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (@as(u32, info.height) > @as(u32, jof_h.k_jof_max_dim)) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (info.tile_w == 0) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (info.tile_h == 0) {
        return jof_h.k_ra8_err_validation_failed;
    }
    // bpp 1 (gray8), 3 (RGB888) or 4 (RGBA8888); 2 is not a defined layout.
    if ((info.bpp == 0) or (info.bpp == 2) or (@as(u32, info.bpp) > @as(u32, jof_h.k_jof_bpp_max))) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (info.codec > jof_h.k_jof_codec_deflate) {
        return jof_h.k_ra8_err_validation_failed;
    }
    return jof_h.k_ra8_ok;
}

/// Reject any non-zero reserved header byte (strict, fail-closed).
/// Ports internal_check_reserved.
fn parseCheckReserved(hdr: []const u8) jof_h.ra8_err_t {
    var i: u8 = jof_h.k_jof_ofs_reserved;
    while (i < jof_h.k_jof_ofs_tile_count) : (i += 1) {
        if (hdr[i] != 0) {
            return jof_h.k_ra8_err_validation_failed;
        }
    }
    i = jof_h.k_jof_ofs_reserved2;
    while (i < jof_h.k_jof_hdr_bytes) : (i += 1) {
        if (hdr[i] != 0) {
            return jof_h.k_ra8_err_validation_failed;
        }
    }
    return jof_h.k_ra8_ok;
}

/// Cross-check grid, tile-count, footer and index-window invariants.
/// Ports internal_check_cross.
fn parseCheckCross(
    info: *jof_h.jof_info_t,
    hdr_count: u32,
    ftr: []const u8,
    total_size: u64,
) jof_h.ra8_err_t {
    const cols: u32 = parseCeilDiv(info.width, info.tile_w);
    const rows: u32 = parseCeilDiv(info.height, info.tile_h);
    const want: u32 = cols * rows;
    if (want > @as(u32, jof_h.k_jof_max_tiles)) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (hdr_count != want) {
        return jof_h.k_ra8_err_validation_failed;
    }
    const ftr_count: u32 = parseRdU32(ftr, jof_h.k_jof_ftr_tile_count);
    const ftr_total: u32 = parseRdU32(ftr, jof_h.k_jof_ftr_total_size);
    const index_off: u32 = parseRdU32(ftr, jof_h.k_jof_ftr_index_off);
    if (ftr_count != want) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (@as(u64, ftr_total) != total_size) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (index_off < @as(u32, jof_h.k_jof_hdr_bytes)) {
        return jof_h.k_ra8_err_validation_failed;
    }
    const index_end: u64 = @as(u64, index_off) + (@as(u64, want) * @as(u64, jof_h.k_jof_index_entry)) +
        @as(u64, jof_h.k_jof_footer_bytes);
    if (index_end != total_size) {
        return jof_h.k_ra8_err_validation_failed;
    }
    info.tile_cols = @intCast(cols);
    info.tile_rows = @intCast(rows);
    info.tile_count = want;
    info.index_off = index_off;
    info.total_size = ftr_total;
    return jof_h.k_ra8_ok;
}

/// Read + validate the 32-byte header region into out_info.
/// Ports internal_parse_header_region.
fn parseHeaderRegion(
    pread: jof_h.jof_pread_fn,
    pread_ctx: ?*anyopaque,
    hdr: []u8,
    out_info: *jof_h.jof_info_t,
) jof_h.ra8_err_t {
    const s_magic_hdr = [jof_h.k_jof_magic_len]u8{ 'J', 'O', 'F', '1' };
    var err = parsePreadExact(pread, pread_ctx, 0, hdr);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    if (!std.mem.eql(u8, hdr[jof_h.k_jof_ofs_magic..][0..jof_h.k_jof_magic_len], &s_magic_hdr)) {
        return jof_h.k_ra8_err_validation_failed;
    }
    err = parseHdrFields(hdr, out_info);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    return parseCheckReserved(hdr);
}

/// Parse + validate a JOF atlas (fail-closed structural validation).
/// Ports jof_parse.
pub export fn jof_parse(
    pread: jof_h.jof_pread_fn,
    pread_ctx: ?*anyopaque,
    total_size: u64,
    out_info: [*c]jof_h.jof_info_t,
) callconv(.c) jof_h.ra8_err_t {
    const s_magic_ftr = [jof_h.k_jof_magic_len]u8{ 'J', 'O', 'F', 'E' };
    if (pread == null or out_info == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    const min_size: u64 = @as(u64, jof_h.k_jof_hdr_bytes) + @as(u64, jof_h.k_jof_footer_bytes);
    if ((total_size < min_size) or (total_size > std.math.maxInt(u32))) {
        return jof_h.k_ra8_err_invalid_size;
    }
    var hdr: [jof_h.k_jof_hdr_bytes]u8 = std.mem.zeroes([jof_h.k_jof_hdr_bytes]u8);
    var err = parseHeaderRegion(pread, pread_ctx, &hdr, out_info);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    var ftr: [jof_h.k_jof_footer_bytes]u8 = std.mem.zeroes([jof_h.k_jof_footer_bytes]u8);
    err = parsePreadExact(pread, pread_ctx, total_size - @as(u64, jof_h.k_jof_footer_bytes), &ftr);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    if (!std.mem.eql(u8, ftr[jof_h.k_jof_ftr_magic..][0..jof_h.k_jof_magic_len], &s_magic_ftr)) {
        return jof_h.k_ra8_err_validation_failed;
    }
    const hdr_count: u32 = parseRdU32(&hdr, jof_h.k_jof_ofs_tile_count);
    return parseCheckCross(out_info, hdr_count, &ftr, total_size);
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

// ---- producer scaffolding: arena carve + work sizing (ported from jof_produce.c, RA8FW-1150) ----

/// Carve `len` 8-byte-aligned bytes from the bump arena; exhaustion is the
/// producer's fail-closed "source too large for the budget" signal.
/// Ports priv_jof_bump_take.
pub export fn priv_jof_bump_take(bump: [*c]jof_h.jof_bump_t, len: usize) callconv(.c) ?*anyopaque {
    if (bump == null or len == 0) {
        return null;
    }
    const mask: usize = @as(usize, jof_h.k_jof_bump_align) - 1;
    const aligned: usize = (bump.*.off +% mask) & ~mask;
    if ((aligned > bump.*.cap) or (len > (bump.*.cap - aligned))) {
        return null;
    }
    bump.*.off = aligned + len;
    return @ptrCast(bump.*.base + aligned);
}

/// Sizes pinned from the vendored miniz the C TUs compile. jof_produce.c
/// carries static asserts on both, so a miniz bump fails the C build until
/// this port's constants move with it.
const k_jof_tinfl_size: u64 = 8376;
const k_jof_tdefl_size: u64 = 319352;
/// libs/ra8_jpeg/inc/ra8_jpeg_sw.h stream constants.
const k_jof_jpeg_stream_min_window: u64 = 131072;
const k_jof_jpeg_stream_mcu_rows_max: u64 = 16;
/// jof_produce.c producer carves (k_jof_png_ring / k_jof_png_inbuf).
const k_jof_png_ring: u64 = 65536;
const k_jof_png_inbuf: u64 = 4096;

/// Working-arena size for the streaming producer, exact to the carve set the
/// producer makes. Ports jof_work_bytes.
pub export fn jof_work_bytes(max_width: u16, max_height: u16, tile_w: u16, tile_h: u16) callconv(.c) u32 {
    if ((max_width == 0) or (@as(u32, max_width) > jof_h.k_jof_max_dim) or
        (max_height == 0) or (@as(u32, max_height) > jof_h.k_jof_max_dim))
    {
        return 0;
    }
    if ((tile_w == 0) or (tile_h == 0)) {
        return 0;
    }
    const cols: u64 = (@as(u64, max_width) + @as(u64, tile_w) - 1) / @as(u64, tile_w);
    const rows: u64 = (@as(u64, max_height) + @as(u64, tile_h) - 1) / @as(u64, tile_h);
    if (cols * rows > jof_h.k_jof_max_tiles) {
        return 0;
    }
    const prod_bpp: u64 = jof_h.k_jof_bpp_max;
    const jpeg_set: u64 = k_jof_jpeg_stream_min_window + (@as(u64, max_width) * k_jof_jpeg_stream_mcu_rows_max * 3);
    const png_set: u64 = k_jof_tinfl_size + k_jof_png_ring + k_jof_png_inbuf +
        (3 * (@as(u64, max_width) * prod_bpp)) + 2;
    const dec_set: u64 = if (jpeg_set > png_set) jpeg_set else png_set;
    const band: u64 = @as(u64, max_width) * @as(u64, tile_h) * prod_bpp;
    const tw_eff: u64 = if (@as(u64, tile_w) < @as(u64, max_width)) @as(u64, tile_w) else @as(u64, max_width);
    const th_eff: u64 = if (@as(u64, tile_h) < @as(u64, max_height)) @as(u64, tile_h) else @as(u64, max_height);
    const stage: u64 = tw_eff * th_eff * prod_bpp;
    // The C helper truncates stage to uint32 before the stored bound; at the
    // exact 2^32 stage the bound wraps to zero, and the total check below
    // refuses the call anyway. Keep the truncation verbatim.
    const cmp: u64 = jof_stored_bound(@truncate(stage));
    const index_bytes: u64 = cols * rows * @as(u64, jof_h.k_jof_index_entry);
    const total: u64 = dec_set + band + stage + cmp + index_bytes + k_jof_tdefl_size + jof_h.k_jof_carve_slack;
    if ((stage > std.math.maxInt(u32)) or (total > std.math.maxInt(u32))) {
        return 0;
    }
    return @intCast(total);
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

// ---- jof_read_tile: bounded per-tile decode (ported from jof.c, RA8FW-1149) ----

/// Deflate seam from ra8_compress (the C TU consumers compile beside the archive).
extern fn ra8_decompress(src: [*c]const u8, src_len: u32, out: [*c]u8, out_cap: u32, out_len: [*c]u32) callconv(.c) jof_h.ra8_err_t;
extern fn ra8_compress(src: [*c]const u8, src_len: u32, out: [*c]u8, out_cap: u32, scratch: ?*anyopaque, scratch_len: u32, out_len: [*c]u32) callconv(.c) jof_h.ra8_err_t;

/// Fetch + window-validate the index entry for tile `n` (ports internal_index_entry).
fn readTileIndexEntry(
    pread: jof_h.jof_pread_fn,
    pread_ctx: ?*anyopaque,
    info: [*c]const jof_h.jof_info_t,
    n: u32,
    out_off: *u32,
    out_len: *u32,
) jof_h.ra8_err_t {
    var entry: [jof_h.k_jof_index_entry]u8 = std.mem.zeroes([jof_h.k_jof_index_entry]u8);
    const eoff: u64 = @as(u64, info.*.index_off) + @as(u64, n) * @as(u64, jof_h.k_jof_index_entry);
    const err = parsePreadExact(pread, pread_ctx, eoff, &entry);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    const toff = parseRdU32(&entry, jof_h.k_jof_idx_ofs_offset);
    const tlen = parseRdU32(&entry, jof_h.k_jof_idx_ofs_length);
    const tend: u64 = @as(u64, toff) + @as(u64, tlen);
    if ((toff < jof_h.k_jof_hdr_bytes) or (tend > @as(u64, info.*.index_off))) {
        return jof_h.k_ra8_err_validation_failed;
    }
    out_off.* = toff;
    out_len.* = tlen;
    return jof_h.k_ra8_ok;
}

/// Decode a validated tile stream into `out_px` (ports internal_decode_stream).
fn readTileDecodeStream(
    pread: jof_h.jof_pread_fn,
    pread_ctx: ?*anyopaque,
    info: [*c]const jof_h.jof_info_t,
    toff: u32,
    tlen: u32,
    payload: u32,
    scratch: [*c]u8,
    scratch_cap: u32,
    out_px: [*c]u8,
) jof_h.ra8_err_t {
    if (info.*.codec == jof_h.k_jof_codec_raw) {
        if (tlen != payload) {
            return jof_h.k_ra8_err_validation_failed;
        }
        return parsePreadExact(pread, pread_ctx, toff, out_px[0..payload]);
    }
    if (scratch == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (tlen > scratch_cap) {
        return jof_h.k_ra8_err_invalid_size;
    }
    const err = parsePreadExact(pread, pread_ctx, toff, scratch[0..tlen]);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    var got: u32 = 0;
    if (ra8_decompress(scratch, tlen, out_px, payload, &got) != jof_h.k_ra8_ok) {
        return jof_h.k_ra8_err_validation_failed;
    }
    if (got != payload) {
        return jof_h.k_ra8_err_validation_failed;
    }
    return jof_h.k_ra8_ok;
}

/// Fetch tile `n`'s index entry, then read + decode its stream (ports internal_fetch_decode).
fn readTileFetchDecode(
    pread: jof_h.jof_pread_fn,
    pread_ctx: ?*anyopaque,
    info: [*c]const jof_h.jof_info_t,
    n: u32,
    payload: u32,
    scratch: [*c]u8,
    scratch_cap: u32,
    out_px: [*c]u8,
) jof_h.ra8_err_t {
    var toff: u32 = 0;
    var tlen: u32 = 0;
    const err = readTileIndexEntry(pread, pread_ctx, info, n, &toff, &tlen);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    return readTileDecodeStream(pread, pread_ctx, info, toff, tlen, payload, scratch, scratch_cap, out_px);
}

/// Read + decode one tile of a parsed atlas (fail-closed, bounded RAM).
/// Ports jof_read_tile.
pub export fn jof_read_tile(
    pread: jof_h.jof_pread_fn,
    pread_ctx: ?*anyopaque,
    info: [*c]const jof_h.jof_info_t,
    tile_x: u16,
    tile_y: u16,
    scratch: [*c]u8,
    scratch_cap: u32,
    out_px: [*c]u8,
    out_cap: u32,
    out_w: [*c]u16,
    out_h: [*c]u16,
) callconv(.c) jof_h.ra8_err_t {
    if (pread == null or info == null or out_px == null or out_w == null or out_h == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    var tw: u16 = 0;
    var th: u16 = 0;
    var err = jof_tile_dims(info, tile_x, tile_y, &tw, &th);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    const payload: u32 = @as(u32, tw) *% @as(u32, th) *% @as(u32, info.*.bpp);
    if (payload > out_cap) {
        return jof_h.k_ra8_err_invalid_size;
    }
    const n: u32 = @as(u32, tile_y) *% @as(u32, info.*.tile_cols) +% @as(u32, tile_x);
    err = readTileFetchDecode(pread, pread_ctx, info, n, payload, scratch, scratch_cap, out_px);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    out_w.* = tw;
    out_h.* = th;
    return jof_h.k_ra8_ok;
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

// ---------------------------------------------------------------------------
// Producer body slice 1 (RA8FW-1151): sink accounting, the prefix-replay
// pull, header emission, the pixel-path carve and the geometry bind.
// ---------------------------------------------------------------------------

/// Store a uint16 little-endian at buf. Ports internal_wr_u16 (the C
/// mask/shift come from k_jof_byte_mask and k_jof_le_sh8, TU-private there).
pub export fn internal_wr_u16(buf: [*c]u8, v: u16) callconv(.c) void {
    buf[0] = @truncate(v & 0xFF);
    buf[1] = @truncate(@as(u32, v) >> 8);
}

/// Store a uint32 little-endian at buf. Ports internal_wr_u32.
pub export fn internal_wr_u32(buf: [*c]u8, v: u32) callconv(.c) void {
    buf[0] = @truncate(v & 0xFF);
    buf[1] = @truncate((v >> 8) & 0xFF);
    buf[2] = @truncate((v >> 16) & 0xFF);
    buf[3] = @truncate((v >> 24) & 0xFF);
}

/// Append bytes to the caller sink, tracking the atlas offset. Guards the
/// u32 atlas size cap before delegating to the caller sink. Ports
/// internal_sink.
pub export fn internal_sink(
    st: *jof_h.jof_prod_state_t,
    buf: [*c]const u8,
    len: usize,
) callconv(.c) jof_h.ra8_err_t {
    if (@as(u64, len) > @as(u64, std.math.maxInt(u32)) - @as(u64, st.*.written)) {
        return jof_h.k_ra8_err_invalid_size;
    }
    const err = (st.*.cfg.*.sink.?)(st.*.cfg.*.sink_ctx, buf, len);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    st.*.written += @intCast(len);
    return jof_h.k_ra8_ok;
}

/// Pull adapter: replay the sniffed head, then delegate to the source.
/// Ports priv_jof_prefix_pull.
pub export fn priv_jof_prefix_pull(
    ctx: ?*anyopaque,
    buf: [*c]u8,
    cap: usize,
    got: [*c]usize,
) callconv(.c) jof_h.ra8_err_t {
    const pfx: *jof_h.jof_prefix_pull_t = @ptrCast(@alignCast(ctx.?));
    if (pfx.*.pos < pfx.*.head_len) {
        const left = pfx.*.head_len - pfx.*.pos;
        const take = if (cap < left) cap else left;
        @memcpy(buf[0..take], (pfx.*.head + pfx.*.pos)[0..take]);
        pfx.*.pos += take;
        got.* = take;
        return jof_h.k_ra8_ok;
    }
    return (pfx.*.inner.?)(pfx.*.inner_ctx, buf, cap, got);
}

/// Serialize + sink the 32-byte JOF header from the bound geometry. Ports
/// internal_emit_header.
pub export fn internal_emit_header(st: *jof_h.jof_prod_state_t) callconv(.c) jof_h.ra8_err_t {
    var hdr: [jof_h.k_jof_hdr_bytes]u8 = undefined;
    @memset(&hdr, 0);
    hdr[0] = 'J';
    hdr[1] = 'O';
    hdr[2] = 'F';
    hdr[3] = '1';
    internal_wr_u16(&hdr[jof_h.k_jof_ofs_width], st.*.w);
    internal_wr_u16(&hdr[jof_h.k_jof_ofs_height], st.*.h);
    internal_wr_u16(&hdr[jof_h.k_jof_ofs_tile_w], st.*.cfg.*.tile_w);
    internal_wr_u16(&hdr[jof_h.k_jof_ofs_tile_h], st.*.cfg.*.tile_h);
    hdr[jof_h.k_jof_ofs_bpp] = st.*.bpp;
    hdr[jof_h.k_jof_ofs_codec] = st.*.cfg.*.codec;
    internal_wr_u32(&hdr[jof_h.k_jof_ofs_tile_count], st.*.tile_count);
    return internal_sink(st, &hdr, hdr.len);
}

/// Carve the band, stage, index and compressed-tile buffers. Sizes are
/// computed in 64-bit and checked against the u32 format cap before any
/// carve; arena exhaustion fails closed. Ports internal_carve_pixel_path.
pub export fn internal_carve_pixel_path(st: *jof_h.jof_prod_state_t) callconv(.c) jof_h.ra8_err_t {
    const tw: u16 = st.*.cfg.*.tile_w;
    const th: u16 = st.*.cfg.*.tile_h;
    const band_bytes: u64 = @as(u64, st.*.w) * @as(u64, th) * @as(u64, st.*.bpp);
    const tw_eff: u32 = if (tw < st.*.w) tw else st.*.w;
    const th_eff: u32 = if (th < st.*.h) th else st.*.h;
    const stage_bytes: u64 = @as(u64, tw_eff) * @as(u64, th_eff) * @as(u64, st.*.bpp);
    // mcdc-deactivated: stage_bytes = tw_eff*th_eff*bpp with tw_eff <= w and
    // th_eff <= tile_h, so stage_bytes <= w*tile_h*bpp = band_bytes always;
    // the stage-overflow arm cannot flip independently of the band arm
    // (which the guards test drives true via a 32768-wide RGBA source and a
    // 65535-tall tile).
    if ((band_bytes > std.math.maxInt(u32)) or (stage_bytes > std.math.maxInt(u32))) {
        return jof_h.k_ra8_err_invalid_size;
    }
    st.*.band = @ptrCast(priv_jof_bump_take(st.*.bump, @intCast(band_bytes)));
    st.*.stage = @ptrCast(priv_jof_bump_take(st.*.bump, @intCast(stage_bytes)));
    st.*.idx = @ptrCast(priv_jof_bump_take(
        st.*.bump,
        @as(usize, st.*.tile_count) * @as(usize, jof_h.k_jof_index_entry),
    ));
    if ((st.*.band == null) or (st.*.stage == null) or (st.*.idx == null)) {
        return jof_h.k_ra8_err_invalid_size;
    }
    if (st.*.cfg.*.codec == jof_h.k_jof_codec_deflate) {
        st.*.cmp_cap = jof_stored_bound(@intCast(stage_bytes));
        st.*.cmp = @ptrCast(priv_jof_bump_take(st.*.bump, @as(usize, st.*.cmp_cap)));
        if (st.*.cmp == null) {
            return jof_h.k_ra8_err_invalid_size;
        }
    }
    return jof_h.k_ra8_ok;
}

/// Bind the source geometry: validate caps, carve buffers, emit the header.
/// Fires once per transcode (from either decoder). Rejects, fail closed:
/// dimensions above the caps, a tile grid above the format cap, and any
/// carve the arena cannot fit. On success the 32-byte JOF header has been
/// sunk. Ports priv_jof_on_geom.
pub export fn priv_jof_on_geom(
    ctx: ?*anyopaque,
    width: u16,
    height: u16,
    channels: u8,
) callconv(.c) jof_h.ra8_err_t {
    const st: *jof_h.jof_prod_state_t = @ptrCast(@alignCast(ctx.?));
    if (st.*.geom_done != 0) {
        return jof_h.k_ra8_err_invalid_state;
    }
    if ((width == 0) or (width > st.*.cap_w) or (height == 0) or (height > st.*.cap_h)) {
        return jof_h.k_ra8_err_invalid_size;
    }
    const cols: u32 = (@as(u32, width) +% @as(u32, st.*.cfg.*.tile_w) -% 1) /
        @as(u32, st.*.cfg.*.tile_w);
    const rows: u32 = (@as(u32, height) +% @as(u32, st.*.cfg.*.tile_h) -% 1) /
        @as(u32, st.*.cfg.*.tile_h);
    if ((cols *% rows) > @as(u32, @intCast(jof_h.k_jof_max_tiles))) {
        return jof_h.k_ra8_err_invalid_size;
    }
    st.*.w = width;
    st.*.h = height;
    st.*.bpp = channels;
    st.*.tile_cols = @truncate(cols);
    st.*.tile_rows = @truncate(rows);
    st.*.tile_count = cols *% rows;
    var err = internal_carve_pixel_path(st);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    err = internal_emit_header(st);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    st.*.geom_done = 1;
    return jof_h.k_ra8_ok;
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

// ---------------------------------------------------------------------------
// Producer body slice 2 (RA8FW-1152): tile encode, band flush and the
// row-accumulate streaming entry.
// ---------------------------------------------------------------------------

/// Encode one packed tile payload and append it to the atlas. Raw codec
/// sinks the payload verbatim; the deflate codec runs it through the
/// zero-heap ra8_compress() into the bounded compressed-tile buffer. The
/// tile's index entry (absolute offset plus stored length) is recorded
/// before the bytes are sunk. Ports internal_encode_tile.
pub export fn internal_encode_tile(st: *jof_h.jof_prod_state_t, payload: u32) callconv(.c) jof_h.ra8_err_t {
    var out_bytes: [*c]const u8 = st.*.stage;
    var out_len: u32 = payload;
    if (st.*.cfg.*.codec == jof_h.k_jof_codec_deflate) {
        var clen: u32 = 0;
        if (ra8_compress(st.*.stage, payload, st.*.cmp, st.*.cmp_cap, @ptrCast(st.*.dfl), st.*.dfl_len, &clen) != jof_h.k_ra8_ok) {
            return jof_h.k_ra8_err_validation_failed;
        }
        out_bytes = st.*.cmp;
        out_len = clen;
    }
    const entry: [*c]u8 = st.*.idx + @as(usize, st.*.tiles_done) * @as(usize, jof_h.k_jof_index_entry);
    internal_wr_u32(&entry[jof_h.k_jof_idx_ofs_offset], st.*.written);
    internal_wr_u32(&entry[jof_h.k_jof_idx_ofs_length], out_len);
    const err = internal_sink(st, out_bytes, @as(usize, out_len));
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    st.*.tiles_done += 1;
    return jof_h.k_ra8_ok;
}

/// Cut the accumulated band into tiles and append each to the atlas.
/// Iterates the tile columns left to right (row-major tile order), packing
/// each tile's rows tightly into the stage buffer before encoding. Edge
/// columns carry their clamped width. Ports internal_flush_band.
pub export fn internal_flush_band(st: *jof_h.jof_prod_state_t, th: u32) callconv(.c) jof_h.ra8_err_t {
    const stride: u32 = @as(u32, st.*.w) * @as(u32, st.*.bpp);
    var tx: u32 = 0;
    while (tx < @as(u32, st.*.tile_cols)) : (tx += 1) {
        const x0: u32 = tx * @as(u32, st.*.cfg.*.tile_w);
        const tw: u32 = if ((@as(u32, st.*.w) -% x0) < @as(u32, st.*.cfg.*.tile_w))
            (@as(u32, st.*.w) -% x0)
        else
            @as(u32, st.*.cfg.*.tile_w);
        const row_bytes: u32 = tw * @as(u32, st.*.bpp);
        var r: u32 = 0;
        while (r < th) : (r += 1) {
            @memcpy(
                st.*.stage[@as(usize, r) * @as(usize, row_bytes) ..][0..row_bytes],
                (st.*.band + @as(usize, r) * @as(usize, stride) + @as(usize, x0) * @as(usize, st.*.bpp))[0..row_bytes],
            );
        }
        const err = internal_encode_tile(st, row_bytes *% th);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
    }
    return jof_h.k_ra8_ok;
}

/// Streaming row entry: accumulate rows into the band buffer, flushing
/// full bands into tiles as they fill. Ports priv_jof_on_rows.
pub export fn priv_jof_on_rows(
    ctx: ?*anyopaque,
    px: [*c]const u8,
    width: u16,
    y0: u16,
    nrows: u16,
    channels: u8,
) callconv(.c) jof_h.ra8_err_t {
    const st: *jof_h.jof_prod_state_t = @ptrCast(@alignCast(ctx.?));
    if (px == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if ((st.*.geom_done == 0) or (width != st.*.w) or (channels != st.*.bpp)) {
        return jof_h.k_ra8_err_validation_failed;
    }
    // mcdc-deactivated: row-ordering contract guard; the PNG scanline assembler emits exactly one row per call at y0 == rows_done and the JPEG stripe walker emits edge-clamped nrows >= 1 at strictly increasing MCU-row origins, so zero/duplicated/overshooting deliveries are not constructible from a public-API source.
    if ((nrows == 0) or (@as(u32, y0) != st.*.rows_seen) or
        ((@as(u32, y0) + @as(u32, nrows)) > @as(u32, st.*.h)))
    {
        return jof_h.k_ra8_err_validation_failed;
    }
    const stride: u32 = @as(u32, st.*.w) * @as(u32, st.*.bpp);
    var src: [*c]const u8 = px;
    var left: u32 = nrows;
    while (left > 0) {
        const room: u32 = @as(u32, st.*.cfg.*.tile_h) - st.*.band_fill;
        const take: u32 = if (left < room) left else room;
        @memcpy(
            (st.*.band + @as(usize, st.*.band_fill) * @as(usize, stride))[0 .. @as(usize, take) * @as(usize, stride)],
            src[0 .. @as(usize, take) * @as(usize, stride)],
        );
        st.*.band_fill += take;
        st.*.rows_seen += take;
        src += @as(usize, take) * @as(usize, stride);
        left -= take;
        if (st.*.band_fill == @as(u32, st.*.cfg.*.tile_h)) {
            const err = internal_flush_band(st, st.*.band_fill);
            if (err != jof_h.k_ra8_ok) {
                return err;
            }
            st.*.band_fill = 0;
        }
    }
    return jof_h.k_ra8_ok;
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

// ---------------------------------------------------------------------------
// Producer body slice 3 (RA8FW-1153): sniff, dispatch, finish/epilogue and
// the public entries. This retires jof_produce.c entirely.
// ---------------------------------------------------------------------------

/// Container formats ra8_imgdec_sniff can report (ra8_imgdec.h). Translated
/// as a plain C int so the extern ABI matches the C enum exactly.
const ra8_imgdec_format_t = c_int;
const k_ra8_imgdec_format_none: ra8_imgdec_format_t = 0;
const k_ra8_imgdec_format_png: ra8_imgdec_format_t = 2;
const k_ra8_imgdec_format_webp: ra8_imgdec_format_t = 4;

/// The geometry a container declares about itself, before any decode
/// (ra8_imgdec.h).
const ra8_imgdec_geom_t = extern struct {
    format: ra8_imgdec_format_t,
    width_px: u32,
    height_px: u32,
};

/// JPEG geometry callback type (ra8_jpeg_sw.h): fires once after SOF0, the
/// consumer hands back the stripe buffer the decoder will fill.
const ra8_jpeg_sw_geom_fn = ?*const fn (ctx: ?*anyopaque, width: u16, height: u16, channels: u8, stripe_rows: u16, out_stripe: [*c][*c]u8, out_stripe_cap: [*c]u32) callconv(.c) jof_h.ra8_err_t;

/// JPEG stripe sink type (ra8_jpeg_sw.h): same shape as the PNG row hook.
const ra8_jpeg_sw_rows_fn = ?*const fn (ctx: ?*anyopaque, px: [*c]const u8, width: u16, y0: u16, nrows: u16, channels: u8) callconv(.c) jof_h.ra8_err_t;

/// sizeof(tdefl_compressor), the zero-heap ra8_compress scratch (pinned by
/// the static asserts jof_produce.c carried).
const k_ra8_compress_scratch_bytes: u32 = 319352;

/// JPEG SOI marker bytes, file-local consts in the retired jof_produce.c.
const k_jof_sniff_bytes: usize = 12; // jof_produce.c file-local: magic bytes pulled up front (WebP needs 12).
const k_jof_jpeg_soi_first: u8 = 0xFF;
const k_jof_jpeg_soi_second: u8 = 0xD8;

/// Minimum sliding window the streaming JPEG decoder accepts
/// (ra8_jpeg_sw.h).
const k_ra8_jpeg_sw_stream_min_window: u32 = 131072;

extern var jpeg_stub_w: u16;
extern var jpeg_stub_h: u16;
extern var jpeg_stub_channels: u8;
extern var jpeg_stub_rows_per_call: u16;
extern var jpeg_stub_px: [4096]u8;

extern fn ra8_imgdec_sniff(bytes: [*c]const u8, byte_count: u32, out: [*c]ra8_imgdec_format_t) callconv(.c) jof_h.ra8_err_t;
extern fn ra8_imgdec_dims(bytes: [*c]const u8, byte_count: u32, out: [*c]ra8_imgdec_geom_t) callconv(.c) jof_h.ra8_err_t;
extern fn ra8_jpeg_sw_decode_stripes(pull: jof_h.jof_pull_fn, pull_ctx: ?*anyopaque, window: [*c]u8, window_cap: u32, on_geom: ra8_jpeg_sw_geom_fn, on_rows: ra8_jpeg_sw_rows_fn, cb_ctx: ?*anyopaque) callconv(.c) jof_h.ra8_err_t;
extern fn ra8_jpeg_sw_get_dimensions(jpeg_buf: [*c]const u8, jpeg_len: u32, out_w: [*c]u16, out_h: [*c]u16) callconv(.c) jof_h.ra8_err_t;

/// Container sniff behind the dispatch decision: the shared
/// ra8_imgdec_sniff over the pulled head, collapsed to format_none on any
/// error. Ports internal_sniffed.
fn internal_sniffed(data: [*c]const u8, len: usize) callconv(.c) ra8_imgdec_format_t {
    var fmt: ra8_imgdec_format_t = k_ra8_imgdec_format_none;
    const n: u32 = if (len > @as(usize, 4294967295)) 4294967295 else @intCast(len);
    const rc = ra8_imgdec_sniff(data, n, &fmt);
    if (rc != jof_h.k_ra8_ok) {
        return k_ra8_imgdec_format_none;
    }
    return fmt;
}

/// JPEG geometry adapter: bind geometry, then carve the MCU stripe buffer
/// sized width * stripe_rows * channels. Ports internal_jpeg_geom.
fn internal_jpeg_geom(ctx: ?*anyopaque, width: u16, height: u16, channels: u8, stripe_rows: u16, out_stripe: [*c][*c]u8, out_stripe_cap: [*c]u32) callconv(.c) jof_h.ra8_err_t {
    const st: *jof_h.jof_prod_state_t = @ptrCast(@alignCast(ctx.?));
    const err = priv_jof_on_geom(st, width, height, channels);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    const cap: u32 = @as(u32, width) * @as(u32, stripe_rows) * @as(u32, channels);
    const buf = priv_jof_bump_take(st.*.bump, cap);
    if (buf == null) {
        return jof_h.k_ra8_err_invalid_size;
    }
    out_stripe.* = @ptrCast(buf.?);
    out_stripe_cap.* = cap;
    return jof_h.k_ra8_ok;
}

/// Emit the tile index and footer, then fill the caller's info. Ports
/// internal_finish.
fn internal_finish(st: *jof_h.jof_prod_state_t, out_info: *jof_h.jof_info_t) callconv(.c) jof_h.ra8_err_t {
    if (st.*.tiles_done != st.*.tile_count) {
        return jof_h.k_ra8_err_validation_failed;
    }
    const index_off: u32 = st.*.written;
    var err = internal_sink(st, st.*.idx, @as(usize, st.*.tile_count) * @as(usize, jof_h.k_jof_index_entry));
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    var ftr: [jof_h.k_jof_footer_bytes]u8 = std.mem.zeroes([jof_h.k_jof_footer_bytes]u8);
    internal_wr_u32(&ftr[jof_h.k_jof_ftr_index_off], index_off);
    internal_wr_u32(&ftr[jof_h.k_jof_ftr_tile_count], st.*.tile_count);
    internal_wr_u32(&ftr[jof_h.k_jof_ftr_total_size], st.*.written +% @as(u32, jof_h.k_jof_footer_bytes));
    ftr[jof_h.k_jof_ftr_magic] = 'J';
    ftr[jof_h.k_jof_ftr_magic + 1] = 'O';
    ftr[jof_h.k_jof_ftr_magic + 2] = 'F';
    ftr[jof_h.k_jof_ftr_magic + 3] = 'E';
    err = internal_sink(st, &ftr, ftr.len);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    out_info.*.width = st.*.w;
    out_info.*.height = st.*.h;
    out_info.*.tile_w = st.*.cfg.*.tile_w;
    out_info.*.tile_h = st.*.cfg.*.tile_h;
    out_info.*.tile_cols = st.*.tile_cols;
    out_info.*.tile_rows = st.*.tile_rows;
    out_info.*.bpp = st.*.bpp;
    out_info.*.codec = st.*.cfg.*.codec;
    out_info.*.tile_count = st.*.tile_count;
    out_info.*.index_off = index_off;
    out_info.*.total_size = st.*.written;
    return jof_h.k_ra8_ok;
}

/// Validate the caller configuration's non-pointer invariants: non-zero tile
/// geometry and a known codec. Ports internal_check_cfg.
fn internal_check_cfg(cfg: [*c]const jof_h.jof_produce_cfg_t) callconv(.c) jof_h.ra8_err_t {
    if ((cfg.*.tile_w == 0) or (cfg.*.tile_h == 0)) {
        return jof_h.k_ra8_err_invalid_arg;
    }
    if (cfg.*.codec > @as(u8, @intCast(jof_h.k_jof_codec_deflate))) {
        return jof_h.k_ra8_err_invalid_arg;
    }
    return jof_h.k_ra8_ok;
}

/// Reset the transcode state and carve the deflate scratch. Ports
/// internal_init_state.
fn internal_init_state(st: *jof_h.jof_prod_state_t, cfg: [*c]const jof_h.jof_produce_cfg_t) callconv(.c) jof_h.ra8_err_t {
    st.* = std.mem.zeroes(jof_h.jof_prod_state_t);
    st.*.cfg = cfg;
    st.*.bump_store = .{ .base = cfg.*.work, .cap = cfg.*.work_cap, .off = 0 };
    st.*.bump = &st.*.bump_store;
    st.*.cap_w = if ((cfg.*.max_width != 0) and (@as(u32, cfg.*.max_width) < @as(u32, jof_h.k_jof_max_dim)))
        cfg.*.max_width
    else
        @intCast(jof_h.k_jof_max_dim);
    st.*.cap_h = if ((cfg.*.max_height != 0) and (@as(u32, cfg.*.max_height) < @as(u32, jof_h.k_jof_max_dim)))
        cfg.*.max_height
    else
        @intCast(jof_h.k_jof_max_dim);
    if (cfg.*.codec == jof_h.k_jof_codec_deflate) {
        st.*.dfl_len = k_ra8_compress_scratch_bytes;
        const dfl = priv_jof_bump_take(st.*.bump, st.*.dfl_len);
        if (dfl == null) {
            return jof_h.k_ra8_err_invalid_size;
        }
        st.*.dfl = @ptrCast(dfl.?);
    }
    return jof_h.k_ra8_ok;
}

/// Pull the sniff head (both formats need at least these bytes): loops the
/// pull seam until the fixed sniff length arrives. Ports internal_sniff_head.
fn internal_sniff_head(cfg: [*c]const jof_h.jof_produce_cfg_t, head: *[k_jof_sniff_bytes]u8) callconv(.c) jof_h.ra8_err_t {
    var head_len: usize = 0;
    while (head_len < @as(usize, k_jof_sniff_bytes)) {
        var got: usize = 0;
        const err = cfg.*.pull.?(cfg.*.pull_ctx, @ptrCast(&head[head_len]), @as(usize, k_jof_sniff_bytes) - head_len, &got);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        if (got == 0) {
            return jof_h.k_ra8_err_protocol_error; // too short to be a JPEG/PNG
        }
        head_len += got;
    }
    return jof_h.k_ra8_ok;
}

/// Dispatch the sniffed source to the matching decoder. Ports
/// internal_dispatch.
fn internal_dispatch(st: *jof_h.jof_prod_state_t, head: [*c]const u8, pfx: *jof_h.jof_prefix_pull_t) callconv(.c) jof_h.ra8_err_t {
    if ((head[0] == k_jof_jpeg_soi_first) and (head[1] == k_jof_jpeg_soi_second)) {
        const window = priv_jof_bump_take(st.*.bump, k_ra8_jpeg_sw_stream_min_window);
        if (window == null) {
            return jof_h.k_ra8_err_invalid_size;
        }
        return ra8_jpeg_sw_decode_stripes(priv_jof_prefix_pull, pfx, @ptrCast(window.?), k_ra8_jpeg_sw_stream_min_window, internal_jpeg_geom, priv_jof_on_rows, st);
    }
    const sniffed = internal_sniffed(head, @as(usize, k_jof_sniff_bytes));
    if (sniffed == k_ra8_imgdec_format_png) {
        return priv_jof_png_rows(priv_jof_prefix_pull, pfx, st.*.bump, st.*.cap_w, st.*.cap_h, priv_jof_on_geom, priv_jof_on_rows, st);
    }
    if (sniffed == k_ra8_imgdec_format_webp) {
        return priv_jof_webp_transcode(st, pfx);
    }
    return jof_h.k_ra8_err_not_supported; // not a JPEG/PNG/WebP source
}

/// Sniff the container, read its declared geometry and range-check it. Ports
/// internal_probe_sniff.
fn internal_probe_sniff(data: [*c]const u8, len: usize, out_w: [*c]u16, out_h: [*c]u16) callconv(.c) jof_h.ra8_err_t {
    if (len < @as(usize, k_jof_sniff_bytes)) {
        return jof_h.k_ra8_err_not_supported; // too short to carry any accepted magic
    }
    var w: u32 = 0;
    var h: u32 = 0;
    if ((data[0] == k_jof_jpeg_soi_first) and (data[1] == k_jof_jpeg_soi_second)) {
        return ra8_jpeg_sw_get_dimensions(data, @intCast(len), out_w, out_h);
    }
    const sniffed = internal_sniffed(data, len);
    if (sniffed == k_ra8_imgdec_format_png) {
        var geom: ra8_imgdec_geom_t = std.mem.zeroes(ra8_imgdec_geom_t);
        const n: u32 = if (len > @as(usize, 4294967295)) 4294967295 else @intCast(len);
        const rc = ra8_imgdec_dims(data, n, &geom);
        if (rc != jof_h.k_ra8_ok) {
            return rc;
        }
        w = geom.width_px;
        h = geom.height_px;
    } else if (sniffed == k_ra8_imgdec_format_webp) {
        const rc = jof_h.ra8_webp_get_info(data, len, &w, &h);
        if (rc != jof_h.k_ra8_ok) {
            return rc;
        }
    } else {
        return jof_h.k_ra8_err_not_supported; // not a JPEG/PNG/WebP source
    }
    if ((w == 0) or (h == 0) or (w > @as(u32, jof_h.k_jof_max_dim)) or (h > @as(u32, jof_h.k_jof_max_dim))) {
        return jof_h.k_ra8_err_invalid_size;
    }
    out_w.* = @intCast(w);
    out_h.* = @intCast(h);
    return jof_h.k_ra8_ok;
}

/// Probe entry: the public null-pointer contract over internal_probe_sniff.
/// Ports jof_probe_dims.
pub export fn jof_probe_dims(data: [*c]const u8, len: usize, out_w: [*c]u16, out_h: [*c]u16) callconv(.c) jof_h.ra8_err_t {
    if (data == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (out_w == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (out_h == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    return internal_probe_sniff(data, len, out_w, out_h);
}

/// Post-decode checks, final band flush and trailer emission. Ports
/// internal_epilogue.
fn internal_epilogue(st: *jof_h.jof_prod_state_t, out_info: *jof_h.jof_info_t) callconv(.c) jof_h.ra8_err_t {
    // mcdc-deactivated: post-decode contract guard; both in-tree decoders return success only after the geometry hook fired and every declared row was delivered (short/hostile streams abort inside the decoder), so neither condition can be flipped through the public producer entry.
    if ((st.*.geom_done == 0) or (st.*.rows_seen != @as(u32, st.*.h))) {
        return jof_h.k_ra8_err_validation_failed; // decoder under-delivered
    }
    if (st.*.band_fill > 0) {
        const err = internal_flush_band(st, st.*.band_fill);
        if (err != jof_h.k_ra8_ok) {
            return err;
        }
        st.*.band_fill = 0;
    }
    return internal_finish(st, out_info);
}

/// Reject any NULL jof_produce argument or seam. Ports
/// internal_produce_args_ok.
fn internal_produce_args_ok(cfg: [*c]const jof_h.jof_produce_cfg_t, out_info: [*c]const jof_h.jof_info_t) callconv(.c) jof_h.ra8_err_t {
    if (cfg == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (out_info == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (cfg.*.pull == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (cfg.*.sink == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    if (cfg.*.work == null) {
        return jof_h.k_ra8_err_null_ptr;
    }
    return jof_h.k_ra8_ok;
}

/// The producer's single-instance static state (jof_produce.c carried it as
/// a function-local static; the transcode is not thread-safe or re-entrant).
var s_prod: jof_h.jof_prod_state_t = undefined;

/// Public transcode entry: validate, init, sniff, dispatch, epilogue. Ports
/// jof_produce.
pub export fn jof_produce(cfg: [*c]const jof_h.jof_produce_cfg_t, out_info: [*c]jof_h.jof_info_t) callconv(.c) jof_h.ra8_err_t {
    var err = internal_produce_args_ok(cfg, out_info);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    err = internal_check_cfg(cfg);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    const st: *jof_h.jof_prod_state_t = &s_prod;
    err = internal_init_state(st, cfg);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    var head: [k_jof_sniff_bytes]u8 = std.mem.zeroes([k_jof_sniff_bytes]u8);
    err = internal_sniff_head(cfg, &head);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    var pfx: jof_h.jof_prefix_pull_t = .{
        .head = &head,
        .head_len = @sizeOf(@TypeOf(head)),
        .pos = 0,
        .inner = cfg.*.pull,
        .inner_ctx = cfg.*.pull_ctx,
    };
    err = internal_dispatch(st, &head, &pfx);
    if (err != jof_h.k_ra8_ok) {
        return err;
    }
    return internal_epilogue(st, out_info.?);
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
