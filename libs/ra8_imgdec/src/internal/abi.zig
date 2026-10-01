//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The record layouts `ra8_imgdec.h`, `ra8_imgdec_backend.h`,
//! `ra8_imgdec_mux.h`, `ra8_imgdec_name.h` and `ra8_imgdec_scratch.h` publish.
//!
//! These are the shapes a C caller allocates and a C backend writes through, so
//! every field here is pinned by a comptime assert below. The asserts are
//! pointer-width aware: the same library is host-tested on a 64-bit machine and
//! linked into a 32-bit ARM image.

const std = @import("std");

/// `ra8_imgdec_caps_t`.
pub const Caps = extern struct {
    formats: u32 = 0,
    pixels: u32 = 0,
    scratch_bytes: u32 = 0,
    scratch_align: u32 = 0,
    dim_max: u32 = 0,
    streams: bool = false,
};

/// `ra8_imgdec_req_t`.
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

/// `ra8_imgdec_image_t`.
pub const Image = extern struct {
    width_px: u32 = 0,
    height_px: u32 = 0,
    stride: u32 = 0,
    used_bytes: u32 = 0,
    format: u32 = 0,
    pixel: u32 = 0,
    had_alpha: bool = false,
};

/// `ra8_imgdec_geom_t`.
pub const Geom = extern struct {
    format: u32 = 0,
    width_px: u32 = 0,
    height_px: u32 = 0,
};

/// `ra8_imgdec_caps_fn`.
pub const CapsFn = *const fn (ctx: ?*anyopaque, out: *Caps) callconv(.c) u16;

/// `ra8_imgdec_decode_fn`.
pub const DecodeFn = *const fn (ctx: ?*anyopaque, req: *const Req, out: *Image) callconv(.c) u16;

/// `struct ra8_imgdec_iface`, the backend vtable.
pub const Iface = extern struct {
    get_caps: ?CapsFn = null,
    decode: ?DecodeFn = null,
};

/// `ra8_imgdec_t`, the caller-allocated handle.
pub const Handle = extern struct {
    iface: ?*const Iface = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_imgdec_name_t`. The two names are NUL-terminated because C reads them;
/// inside the library the table holds sentinel-terminated slices.
pub const Name = extern struct {
    format: u32 = 0,
    ext: ?[*:0]const u8 = null,
    mime: ?[*:0]const u8 = null,
};

/// `ra8_imgdec_scratch_t`, the caller-allocated bump arena.
pub const Scratch = extern struct {
    base: ?[*]u8 = null,
    cap: usize = 0,
    offset: usize = 0,
    live: usize = 0,
    high_water: usize = 0,
};

/// `k_ra8_imgdec_mux_max`: one member per backend named in RA8FW-308.
pub const mux_max: u32 = 4;

/// `ra8_imgdec_mux_t`.
pub const Mux = extern struct {
    members: [mux_max]Handle = @splat(.{}),
    count: u32 = 0,
};

const ptr_bytes = @sizeOf(usize);

comptime {
    // Five uint32 and a bool, padded to the uint32 alignment.
    std.debug.assert(@sizeOf(Caps) == 24);
    std.debug.assert(@offsetOf(Caps, "dim_max") == 16);
    std.debug.assert(@offsetOf(Caps, "streams") == 20);

    // Three pointers and five uint32; on a 64-bit host `byte_count` is
    // followed by four bytes of padding before `arena`.
    std.debug.assert(@offsetOf(Req, "byte_count") == ptr_bytes);
    std.debug.assert(@offsetOf(Req, "arena") == 2 * ptr_bytes);
    std.debug.assert(@offsetOf(Req, "dst") == 3 * ptr_bytes);
    std.debug.assert(@sizeOf(Req) == 4 * ptr_bytes + 16);

    std.debug.assert(@sizeOf(Image) == 28);
    std.debug.assert(@offsetOf(Image, "had_alpha") == 24);

    std.debug.assert(@sizeOf(Geom) == 12);
    std.debug.assert(@offsetOf(Geom, "height_px") == 8);

    std.debug.assert(@sizeOf(Iface) == 2 * ptr_bytes);
    std.debug.assert(@sizeOf(Handle) == 2 * ptr_bytes);
    std.debug.assert(@offsetOf(Handle, "ctx") == ptr_bytes);

    // A uint32 then two pointers: the enum is padded up to pointer alignment.
    std.debug.assert(@offsetOf(Name, "ext") == ptr_bytes);
    std.debug.assert(@sizeOf(Name) == 3 * ptr_bytes);

    std.debug.assert(@sizeOf(Scratch) == 5 * ptr_bytes);
    std.debug.assert(@offsetOf(Scratch, "high_water") == 4 * ptr_bytes);

    std.debug.assert(@offsetOf(Mux, "count") == mux_max * 2 * ptr_bytes);
}
