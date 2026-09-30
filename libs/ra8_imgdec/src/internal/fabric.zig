//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The seam itself: validate a request, refuse anything the bound backend did
//! not advertise, and only then dispatch.
//!
//! These entries return a bare `ra8_err_t` rather than a Zig error set. A
//! backend's `get_caps` or `decode` may answer with any code in the tree and
//! the fabric hands it back untranslated, which a closed error set cannot do.

const abi = @import("abi.zig");
const arena = @import("arena.zig");
const dims_mod = @import("dims.zig");
const sniff_mod = @import("sniff.zig");
const vocab = @import("vocab.zig");

const Caps = abi.Caps;
const Err = vocab.Err;
const Format = vocab.Format;
const Geom = abi.Geom;
const Handle = abi.Handle;
const Image = abi.Image;
const Pixel = vocab.Pixel;
const Req = abi.Req;

/// The ring-below seam, re-exported so a caller binds one without reaching
/// past this file.
pub const Ops = arena.Ops;

/// The backend's capability record, rejected unless it describes something
/// usable: a non-empty subset of each mask and a size limit within the
/// fabric's own.
pub fn fetchCaps(dec: *const Handle, out: *Caps) u16 {
    out.* = .{};

    const iface = dec.iface orelse return Err.not_initialized;
    const get_caps = iface.get_caps orelse return Err.invalid_state;

    var caps: Caps = .{};
    const err = get_caps(dec.ctx, &caps);
    if (err != Err.ok) return err;

    const formats_ok = caps.formats != 0 and (caps.formats & ~Format.mask) == 0;
    const pixels_ok = caps.pixels != 0 and (caps.pixels & ~Pixel.mask) == 0;
    const dim_ok = caps.dim_max != 0 and caps.dim_max <= vocab.Limits.dim_max;
    if (!formats_ok or !pixels_ok or !dim_ok) return Err.invalid_state;

    out.* = caps;
    return Err.ok;
}

/// Whether the backend opens `format` into `pixel`.
pub fn supports(dec: *const Handle, format: u32, pixel: u32, out_ok: *bool) u16 {
    out_ok.* = false;

    if (!vocab.oneDefinedBit(format, Format.mask) or !vocab.oneDefinedBit(pixel, Pixel.mask)) {
        return Err.invalid_arg;
    }

    var caps: Caps = .{};
    const err = fetchCaps(dec, &caps);
    if (err != Err.ok) return err;

    out_ok.* = (format & caps.formats) != 0 and (pixel & caps.pixels) != 0;
    return Err.ok;
}

/// The argument contract a backend is spared re-checking.
fn checkReq(req: *const Req) u16 {
    if (req.bytes == null or req.dst == null) return Err.null_ptr;
    if (req.byte_count == 0 or req.dst_bytes == 0) return Err.invalid_size;
    if (!vocab.oneDefinedBit(req.want, Pixel.mask)) return Err.invalid_arg;
    if (req.format != Format.none and !vocab.oneDefinedBit(req.format, Format.mask)) {
        return Err.invalid_arg;
    }

    const bpp = vocab.pixelBytes(req.want);
    if (req.dst_stride != 0 and req.dst_stride < bpp) return Err.invalid_size;
    if (req.dst_bytes < bpp) return Err.invalid_size;
    return Err.ok;
}

/// A declared format is checked, never trusted; `Format.none` asks the fabric
/// to sniff instead.
fn resolveFormat(req: *const Req, out: *u32) u16 {
    if (req.format != Format.none) {
        out.* = req.format;
        return Err.ok;
    }

    const bytes = req.bytes.?[0..req.byte_count];
    out.* = sniff_mod.sniff(bytes) catch return Err.not_supported;
    return Err.ok;
}

/// The capability gate, including the scratch the backend said it needs.
fn checkAgainstCaps(req: *const Req, caps: *const Caps, format: u32, ops: arena.Ops) u16 {
    if ((req.want & caps.pixels) == 0) return Err.not_supported;
    if ((format & caps.formats) == 0) return Err.not_supported;
    if (caps.scratch_bytes == 0) return Err.ok;

    if (req.arena == null) return Err.invalid_state;

    var remaining: u32 = 0;
    const err = ops.remaining(req.arena, &remaining);
    if (err != Err.ok) return err;
    if (remaining < caps.scratch_bytes) return Err.no_mem;
    return Err.ok;
}

/// Decode `req` through the bound backend.
pub fn decode(dec: *const Handle, req: *const Req, out: *Image, ops: arena.Ops) u16 {
    out.* = .{};

    var caps: Caps = .{};
    const caps_err = fetchCaps(dec, &caps);
    if (caps_err != Err.ok) return caps_err;

    const decode_fn = dec.iface.?.decode orelse return Err.invalid_state;

    const req_err = checkReq(req);
    if (req_err != Err.ok) return req_err;

    var format: u32 = Format.none;
    const format_err = resolveFormat(req, &format);
    if (format_err != Err.ok) return format_err;

    const gate_err = checkAgainstCaps(req, &caps, format, ops);
    if (gate_err != Err.ok) return gate_err;

    var resolved = req.*;
    resolved.format = format;

    const err = decode_fn(dec.ctx, &resolved, out);
    if (err != Err.ok) out.* = .{};
    return err;
}

/// Can this backend take these bytes, and how big does the image say it is.
///
/// `dim_max` is enforced here and nowhere else: `decode` has no geometry until
/// a backend has already parsed the header.
pub fn probe(dec: *const Handle, bytes: []const u8, out: *Geom) u16 {
    out.* = .{};

    var caps: Caps = .{};
    const caps_err = fetchCaps(dec, &caps);
    if (caps_err != Err.ok) return caps_err;

    const geom = dims_mod.dims(bytes) catch |fault| {
        // Two doors into the seam answer unrecognised bytes the same way.
        if (fault == vocab.Fault.NotFound) return Err.not_supported;
        return vocab.faultCode(fault);
    };

    if ((geom.format & caps.formats) == 0) return Err.not_supported;
    if (geom.width_px > caps.dim_max or geom.height_px > caps.dim_max) {
        return Err.not_supported;
    }

    out.* = geom;
    return Err.ok;
}
