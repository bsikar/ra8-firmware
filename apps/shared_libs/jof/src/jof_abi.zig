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
