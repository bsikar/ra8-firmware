//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_imgdec` backend over the vendored libwebp facade, ported from
//! src/ra8_webp_imgdec.c (RA8FW-1209, slice 2; the RA8FW-308 adapter). The
//! vtable, the caps record and the decode hook behind the unchanged C header
//! inc/ra8_webp_imgdec.h. The hook is a thin adapter: it re-checks the
//! container it was told it holds, pre-flights the geometry and the
//! destination, hands the bound arena to ra8_webp_decode_rgba(), and reads
//! the source's alpha flag out of the container header. No decoding logic is
//! added or duplicated here. C source status: ra8_webp_imgdec.c stays on dev
//! until the package flip slice.

const std = @import("std");
const arena = @import("ra8_webp_arena_abi.zig");
const Scratch = arena.Scratch;

const ok: u16 = 0;
const err_invalid_state: u16 = 0x104;
const err_invalid_size: u16 = 0x105;
const err_not_supported: u16 = 0x107;
const err_validation_failed: u16 = 0x501;
const err_null_ptr: u16 = 0x504;

/// The one format this backend opens (k_ra8_imgdec_format_webp).
const format_webp: u32 = 1 << 2;
/// The one layout ra8_webp_decode_rgba() writes (k_ra8_imgdec_pixel_rgba8888).
const pixel_rgba8888: u32 = 1 << 2;
/// Max accepted width/height, pixels/axis (k_ra8_webp_max_dim).
const max_dim: u32 = 8192;
/// Output bytes per pixel, RGBA8888 (k_ra8_webp_bytes_per_px).
const bytes_per_px: u32 = 4;

/// Container offsets and sizes this adapter reads (k_webpdec_* in the C).
const chunk_ofs: u32 = 12; // First chunk FourCC, past RIFF+size+WEBP.
const body_ofs: u32 = 20; // First chunk payload, past FourCC+size.
const vp8x_flag_ofs: u32 = 20; // VP8X feature-flags byte.
const vp8x_min: u32 = 21; // Bytes needed to read that flags byte.
const vp8l_min: u32 = 25; // Bytes needed to read VP8L's packed word.
const alpha_bit: u8 = 0x10; // VP8X flags: ALPH present.
const l8_sig: u8 = 0x2F; // VP8L signature byte at the payload head.
const l8_shift: u5 = 28; // VP8L packed word: alpha_is_used bit.

const tag: [*:0]const u8 = "ra8_webp_imgdec";

extern fn ra8_log_emit_error(t: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_imgdec_dims(bytes: ?[*]const u8, byte_count: u32, out: ?*Geom) callconv(.c) u16;
extern fn ra8_imgdec_pixel_bytes(pixel: u32) callconv(.c) u32;
extern fn ra8_webp_decode_rgba(data: ?[*]const u8, size: usize, arena_ptr: ?*Scratch, out_rgba: ?[*]u8, out_stride: usize, out_capacity: usize, out_w: ?*u32, out_h: ?*u32) callconv(.c) u16;

fn logError(message: [*:0]const u8) void {
    ra8_log_emit_error(tag, message);
}

/// Layout mirrors of the ra8_imgdec ABI (libs/ra8_imgdec/inc/ra8_imgdec.h,
/// ra8_imgdec_backend.h), kept as extern structs so the vtable crosses the
/// C ABI without importing across package roots.
pub const Caps = extern struct {
    formats: u32 = 0,
    pixels: u32 = 0,
    scratch_bytes: u32 = 0,
    scratch_align: u32 = 0,
    dim_max: u32 = 0,
    streams: bool = false,
};

pub const Req = extern struct {
    bytes: ?[*]const u8 = null,
    byte_count: u32 = 0,
    arena: ?*anyopaque = null,
    dst: ?[*]u8 = null,
    dst_bytes: u32 = 0,
    dst_stride: u32 = 0,
    format: u32 = 0,
    want: u32 = 0,
};

pub const Image = extern struct {
    width_px: u32 = 0,
    height_px: u32 = 0,
    stride: u32 = 0,
    used_bytes: u32 = 0,
    format: u32 = 0,
    pixel: u32 = 0,
    had_alpha: bool = false,
};

pub const Geom = extern struct {
    format: u32 = 0,
    width_px: u32 = 0,
    height_px: u32 = 0,
};

pub const Iface = extern struct {
    get_caps: *const fn (?*anyopaque, ?*Caps) callconv(.c) u16,
    decode: *const fn (?*anyopaque, ?*const Req, ?*Image) callconv(.c) u16,
};

pub const Dec = extern struct {
    iface: ?*const Iface = null,
    ctx: ?*anyopaque = null,
};

comptime {
    // The whole surface is measured in 32 bits, so the widest frame this
    // backend admits must fit there at RGBA8888.
    if (@as(u64, max_dim) * @as(u64, max_dim) * @as(u64, bytes_per_px) > std.math.maxInt(u32)) {
        @compileError("ra8_webp_imgdec: a dim_max surface must not overflow a 32-bit byte count");
    }
    // The facade takes its lengths as usize but converts the stride to c_int,
    // so a request this adapter accepts must be expressible there.
    if (@sizeOf(usize) < @sizeOf(u32)) {
        @compileError("ra8_webp_imgdec: a request length must fit a usize");
    }
}

/// Read one little-endian 32-bit word out of the container.
fn le32(p: [*]const u8) u32 {
    return @as(u32, p[0]) | (@as(u32, p[1]) << 8) | (@as(u32, p[2]) << 16) | (@as(u32, p[3]) << 24);
}

/// Verify the container and read its declared geometry, in one pass. Both
/// duties the fabric leaves to a backend are answered by ra8_imgdec_dims(),
/// so this asks it once rather than sniffing and then probing. The container
/// check is not ceremony: ra8_webp_get_info() would refuse a PNG anyway, but
/// refusing it here means a consumer routing by format gets the same
/// not_supported from every backend rather than one facade's validation_failed.
fn probe(req: *const Req, out_w: *u32, out_h: *u32) u16 {
    var geom: Geom = .{};
    const err = ra8_imgdec_dims(req.bytes, req.byte_count, &geom);
    if (err != ok) {
        return err;
    }
    if (geom.format != req.format) {
        return err_not_supported;
    }
    if (geom.width_px > max_dim or geom.height_px > max_dim) {
        return err_invalid_size;
    }
    out_w.* = geom.width_px;
    out_h.* = geom.height_px;
    return ok;
}

/// Decide whether the request's destination can take the surface. The facade
/// decodes straight into req.dst, and it demands a capacity of height*stride
/// rather than the (height-1)*stride+packed the last row actually occupies.
/// That is the facade's contract, so this adapter applies the same bar instead
/// of passing a smaller capacity and letting the facade refuse.
fn dstOk(req: *const Req, rows: u32, packed_row: u32, out_span: *u32, out_used: *u32) u16 {
    const stride: u32 = if (req.dst_stride != 0) req.dst_stride else packed_row;
    if (stride < packed_row) {
        return err_invalid_size;
    }
    const span = @as(u64, rows) * @as(u64, stride);
    if (span > @as(u64, req.dst_bytes)) {
        return err_invalid_size;
    }
    out_span.* = @intCast(span);
    out_used.* = @intCast(@as(u64, rows - 1) * @as(u64, stride) + @as(u64, packed_row));
    return ok;
}

/// Report whether the source container declares an alpha channel. The facade
/// always writes RGBA8888, so the destination has an alpha byte whatever the
/// source was; had_alpha is the fabric's field for whether that byte carries
/// anything. WebP states it in the header rather than implying it: an
/// extended file (VP8X) sets the ALPH feature flag, a lossless file (VP8L)
/// sets alpha_is_used in its packed word, and a plain lossy `VP8 ` stream has
/// no alpha at all.
fn hadAlpha(bytes: [*]const u8, count: u32) bool {
    if (count < body_ofs) {
        return false;
    }
    const t = bytes + @as(usize, chunk_ofs);
    if (t[0] == 'V' and t[1] == 'P' and t[2] == '8') {
        if (t[3] == 'X' and count >= vp8x_min) {
            return (bytes[@as(usize, vp8x_flag_ofs)] & alpha_bit) != 0;
        }
        if (t[3] == 'L' and count >= vp8l_min and bytes[@as(usize, body_ofs)] == l8_sig) {
            const word = le32(bytes + @as(usize, body_ofs) + 1);
            return ((word >> l8_shift) & 1) != 0;
        }
    }
    return false;
}

/// Report this backend's static capabilities. The arena came through the
/// binder, not the request, so scratch_bytes is 0: this backend draws nothing
/// from req.arena.
fn internalCaps(ctx: ?*anyopaque, out: ?*Caps) callconv(.c) u16 {
    _ = ctx;
    out.?.* = .{
        .formats = format_webp,
        .pixels = pixel_rgba8888,
        .scratch_bytes = 0,
        .scratch_align = 0,
        .dim_max = max_dim,
        .streams = false,
    };
    return ok;
}

/// Decode one WebP into the request's destination surface. Verifies the
/// container, pre-flights the geometry and the destination, then calls
/// ra8_webp_decode_rgba() with the arena recorded at bind time. The facade
/// resets that arena on entry and drains it before returning on every path,
/// so no handle is left holding the previous decode's store.
fn internalDecode(ctx: ?*anyopaque, req_ptr: ?*const Req, out: ?*Image) callconv(.c) u16 {
    const arena_ptr: ?*Scratch = @ptrCast(@alignCast(ctx));
    if (arena_ptr == null) {
        return err_invalid_state;
    }
    const req = req_ptr.?;

    var src_w: u32 = 0;
    var src_h: u32 = 0;
    const geom = probe(req, &src_w, &src_h);
    if (geom != ok) {
        logError("decode: header rejected");
        return geom;
    }

    const bpp = ra8_imgdec_pixel_bytes(req.want);
    const packed_row: u32 = src_w * bpp;
    const stride: u32 = if (req.dst_stride != 0) req.dst_stride else packed_row;
    var span: u32 = 0;
    var used: u32 = 0;
    const fits = dstOk(req, src_h, packed_row, &span, &used);
    if (fits != ok) {
        logError("decode: destination cannot hold the surface");
        return fits;
    }

    var got_w: u32 = 0;
    var got_h: u32 = 0;
    const err = ra8_webp_decode_rgba(req.bytes, req.byte_count, arena_ptr, req.dst, stride, span, &got_w, &got_h);
    if (err != ok) {
        logError("decode: the facade refused the image");
        return err;
    }

    // The probe read the header; libwebp read it again on its way through the
    // body. A disagreement means the surface just written is not the one the
    // caller sized, so it is refused rather than reported.
    if (got_w != src_w or got_h != src_h) {
        logError("decode: decoded geometry disagrees with the header");
        return err_validation_failed;
    }

    out.?.* = .{
        .width_px = src_w,
        .height_px = src_h,
        .stride = stride,
        .used_bytes = used,
        .format = req.format,
        .pixel = req.want,
        .had_alpha = hadAlpha(req.bytes.?, req.byte_count),
    };
    return ok;
}

/// The one vtable instance; never per handle.
const s_iface = Iface{ .get_caps = internalCaps, .decode = internalDecode };

/// Bind the libwebp facade as an ra8_imgdec_t backend: fills out with this
/// module's vtable and records scratch as the arena every decode through out
/// will bump. scratch is not touched here and not required to be empty.
pub export fn ra8_webp_imgdec_bind(out: ?*Dec, scratch: ?*Scratch) callconv(.c) u16 {
    if (out == null) {
        logError("bind: null handle");
        return err_null_ptr;
    }
    if (scratch == null) {
        logError("bind: null scratch");
        return err_null_ptr;
    }
    out.?.iface = &s_iface;
    out.?.ctx = scratch;
    return ok;
}
