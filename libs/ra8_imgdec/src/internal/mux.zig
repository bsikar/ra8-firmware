//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A set of bound decoders answered as one: the caller names a format and a
//! destination layout, and the mux picks the member that advertises both.
//!
//! Like the fabric, these entries return a bare `ra8_err_t`, because a member's
//! own error code travels through them unchanged.

const abi = @import("abi.zig");
const arena = @import("arena.zig");
const fabric = @import("fabric.zig");
const sniff_mod = @import("sniff.zig");
const vocab = @import("vocab.zig");

const Caps = abi.Caps;
const Err = vocab.Err;
const Format = vocab.Format;
const Geom = abi.Geom;
const Handle = abi.Handle;
const Image = abi.Image;
const Mux = abi.Mux;
const Pixel = vocab.Pixel;
const Req = abi.Req;

/// The ring-below seam, re-exported for the same reason the fabric does it.
pub const Ops = arena.Ops;

fn usable(mux: *const Mux) u16 {
    if (mux.count == 0) return Err.not_initialized;
    return Err.ok;
}

fn capsCover(caps: *const Caps, format: u32, pixel: u32) bool {
    return (format & caps.formats) != 0 and (pixel & caps.pixels) != 0;
}

/// The first member advertising both, or null with `Err.ok` when none does.
fn find(mux: *const Mux, format: u32, pixel: u32, out: *?*const Handle) u16 {
    out.* = null;

    for (mux.members[0..mux.count]) |*member| {
        var caps: Caps = .{};
        const err = fabric.fetchCaps(member, &caps);
        if (err != Err.ok) return err;
        if (capsCover(&caps, format, pixel)) {
            out.* = member;
            return Err.ok;
        }
    }
    return Err.ok;
}

fn pairOk(mux: *const Mux, format: u32, pixel: u32) u16 {
    if (!vocab.oneDefinedBit(format, Format.mask) or !vocab.oneDefinedBit(pixel, Pixel.mask)) {
        return Err.invalid_arg;
    }
    return usable(mux);
}

fn routeReqOk(req: *const Req) u16 {
    if (req.bytes == null or req.dst == null) return Err.null_ptr;
    if (req.byte_count == 0) return Err.invalid_size;
    if (!vocab.oneDefinedBit(req.want, Pixel.mask)) return Err.invalid_arg;
    if (req.format != Format.none and !vocab.oneDefinedBit(req.format, Format.mask)) {
        return Err.invalid_arg;
    }
    return Err.ok;
}

fn sniffFormat(bytes: []const u8, out: *u32) u16 {
    out.* = sniff_mod.sniff(bytes) catch return Err.not_supported;
    return Err.ok;
}

fn resolve(req: *const Req, out: *u32) u16 {
    if (req.format != Format.none) {
        out.* = req.format;
        return Err.ok;
    }
    return sniffFormat(req.bytes.?[0..req.byte_count], out);
}

pub fn init(mux: *Mux) u16 {
    mux.* = .{};
    return Err.ok;
}

/// Take a copy of `dec`, once its capability record has proven readable.
pub fn add(mux: *Mux, dec: *const Handle) u16 {
    if (mux.count >= abi.mux_max) return Err.no_mem;

    var caps: Caps = .{};
    const err = fabric.fetchCaps(dec, &caps);
    if (err != Err.ok) return err;

    mux.members[mux.count] = dec.*;
    mux.count += 1;
    return Err.ok;
}

/// Every container the set opens into `pixel`.
pub fn formats(mux: *const Mux, pixel: u32, out_formats: *u32) u16 {
    if (!vocab.oneDefinedBit(pixel, Pixel.mask)) return Err.invalid_arg;

    const ready = usable(mux);
    if (ready != Err.ok) return ready;

    var openable: u32 = 0;
    for (mux.members[0..mux.count]) |*member| {
        var caps: Caps = .{};
        const err = fabric.fetchCaps(member, &caps);
        if (err != Err.ok) return err;
        if ((pixel & caps.pixels) != 0) openable |= caps.formats;
    }

    out_formats.* = openable;
    return Err.ok;
}

pub fn supports(mux: *const Mux, format: u32, pixel: u32, out_ok: *bool) u16 {
    out_ok.* = false;

    const ready = pairOk(mux, format, pixel);
    if (ready != Err.ok) return ready;

    var member: ?*const Handle = null;
    const err = find(mux, format, pixel, &member);
    if (err != Err.ok) return err;

    out_ok.* = member != null;
    return Err.ok;
}

pub fn route(mux: *const Mux, format: u32, pixel: u32, out: *?*const Handle) u16 {
    out.* = null;

    const ready = pairOk(mux, format, pixel);
    if (ready != Err.ok) return ready;

    var member: ?*const Handle = null;
    const err = find(mux, format, pixel, &member);
    if (err != Err.ok) return err;

    out.* = member orelse return Err.not_supported;
    return Err.ok;
}

pub fn decode(mux: *const Mux, req: *const Req, out: *Image, ops: arena.Ops) u16 {
    out.* = .{};

    const ready = usable(mux);
    if (ready != Err.ok) return ready;

    const req_err = routeReqOk(req);
    if (req_err != Err.ok) return req_err;

    var format: u32 = Format.none;
    const format_err = resolve(req, &format);
    if (format_err != Err.ok) return format_err;

    var member: ?*const Handle = null;
    const route_err = route(mux, format, req.want, &member);
    if (route_err != Err.ok) return route_err;

    var resolved = req.*;
    resolved.format = format;
    return fabric.decode(member.?, &resolved, out, ops);
}

/// The widest scratch demand in the set, and the strictest alignment.
pub fn scratchBudget(mux: *const Mux, out_bytes: *u32, out_align: *u32) u16 {
    const ready = usable(mux);
    if (ready != Err.ok) return ready;

    var bytes: u32 = 0;
    var alignment: u32 = 0;
    for (mux.members[0..mux.count]) |*member| {
        var caps: Caps = .{};
        const err = fabric.fetchCaps(member, &caps);
        if (err != Err.ok) return err;
        if (caps.scratch_bytes > bytes) bytes = caps.scratch_bytes;
        if (caps.scratch_align > alignment) alignment = caps.scratch_align;
    }

    out_bytes.* = bytes;
    out_align.* = alignment;
    return Err.ok;
}

pub fn probe(
    mux: *const Mux,
    bytes: []const u8,
    pixel: u32,
    out_geom: *Geom,
    out_member: ?*?*const Handle,
) u16 {
    out_geom.* = .{};
    if (out_member) |slot| slot.* = null;

    if (!vocab.oneDefinedBit(pixel, Pixel.mask)) return Err.invalid_arg;

    const ready = usable(mux);
    if (ready != Err.ok) return ready;
    if (bytes.len == 0) return Err.invalid_size;

    var format: u32 = Format.none;
    const format_err = sniffFormat(bytes, &format);
    if (format_err != Err.ok) return format_err;

    var member: ?*const Handle = null;
    const route_err = route(mux, format, pixel, &member);
    if (route_err != Err.ok) return route_err;

    var geom: Geom = .{};
    const err = fabric.probe(member.?, bytes, &geom);
    if (err != Err.ok) return err;

    out_geom.* = geom;
    if (out_member) |slot| slot.* = member;
    return Err.ok;
}
