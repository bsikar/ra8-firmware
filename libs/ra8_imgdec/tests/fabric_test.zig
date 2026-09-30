//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The seam: what it refuses before a backend is ever reached, and what it
//! hands a backend once it has.

const std = @import("std");
const fabric = @import("fabric");

const Caps = extern struct {
    formats: u32 = 0,
    pixels: u32 = 0,
    scratch_bytes: u32 = 0,
    scratch_align: u32 = 0,
    dim_max: u32 = 0,
    streams: bool = false,
};

const Req = extern struct {
    bytes: ?[*]const u8 = null,
    byte_count: u32 = 0,
    arena: ?*anyopaque = null,
    dst: ?[*]u8 = null,
    dst_bytes: u32 = 0,
    dst_stride: u32 = 0,
    format: u32 = 0,
    want: u32 = 0,
};

const Image = extern struct {
    width_px: u32 = 0,
    height_px: u32 = 0,
    stride: u32 = 0,
    used_bytes: u32 = 0,
    format: u32 = 0,
    pixel: u32 = 0,
    had_alpha: bool = false,
};

const Geom = extern struct { format: u32 = 0, width_px: u32 = 0, height_px: u32 = 0 };

const Iface = extern struct {
    get_caps: ?*const fn (ctx: ?*anyopaque, out: *Caps) callconv(.c) u16 = null,
    decode: ?*const fn (ctx: ?*anyopaque, req: *const Req, out: *Image) callconv(.c) u16 = null,
};

const Handle = extern struct { iface: ?*const Iface = null, ctx: ?*anyopaque = null };

const Format = struct {
    const none: u32 = 0;
    const jpeg: u32 = 1 << 0;
    const png: u32 = 1 << 1;
    const webp: u32 = 1 << 2;
    const gif: u32 = 1 << 3;
    const bmp: u32 = 1 << 4;
    const tga: u32 = 1 << 5;
};

const Pixel = struct {
    const grey8: u32 = 1 << 0;
    const rgb888: u32 = 1 << 1;
    const rgba8888: u32 = 1 << 2;
};

const Err = struct {
    const ok: u16 = 0;
    const no_mem: u16 = 0x102;
    const invalid_arg: u16 = 0x103;
    const invalid_state: u16 = 0x104;
    const invalid_size: u16 = 0x105;
    const not_supported: u16 = 0x107;
    const not_initialized: u16 = 0x10F;
    const null_ptr: u16 = 0x504;
};

/// What a fake backend answers with, and what it recorded being asked.
const Backend = struct {
    caps: Caps = .{},
    caps_err: u16 = Err.ok,
    decode_err: u16 = Err.ok,
    produced: Image = .{},
    saw_format: u32 = Format.none,
    decode_calls: u32 = 0,

    fn getCaps(ctx: ?*anyopaque, out: *Caps) callconv(.c) u16 {
        const self: *Backend = @ptrCast(@alignCast(ctx.?));
        out.* = self.caps;
        return self.caps_err;
    }

    fn decode(ctx: ?*anyopaque, req: *const Req, out: *Image) callconv(.c) u16 {
        const self: *Backend = @ptrCast(@alignCast(ctx.?));
        self.decode_calls += 1;
        self.saw_format = req.format;
        out.* = self.produced;
        return self.decode_err;
    }
};

const full_iface = Iface{ .get_caps = Backend.getCaps, .decode = Backend.decode };

fn handleFor(backend: *Backend) Handle {
    return .{ .iface = &full_iface, .ctx = backend };
}

/// A backend that opens PNG into RGBA and needs no scratch.
fn plainBackend() Backend {
    return .{ .caps = .{
        .formats = Format.png,
        .pixels = Pixel.rgba8888,
        .dim_max = 4096,
    } };
}

const png_head = [_]u8{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

fn pngOf(width: u32, height: u32) [24]u8 {
    var out = [_]u8{0} ** 24;
    @memcpy(out[0..8], &png_head);
    @memcpy(out[12..16], "IHDR");
    std.mem.writeInt(u32, out[16..20], width, .big);
    std.mem.writeInt(u32, out[20..24], height, .big);
    return out;
}

var arena_seen: u32 = 0;

fn remainingPlenty(arena_ptr: ?*anyopaque, out: *u32) u16 {
    _ = arena_ptr;
    arena_seen += 1;
    out.* = 1 << 20;
    return Err.ok;
}

fn remainingTiny(arena_ptr: ?*anyopaque, out: *u32) u16 {
    _ = arena_ptr;
    out.* = 8;
    return Err.ok;
}

fn remainingFails(arena_ptr: ?*anyopaque, out: *u32) u16 {
    _ = arena_ptr;
    out.* = 0;
    return 0x4343;
}

fn carveRefuses(a: ?*anyopaque, b: u32, c: u32, out: *?*anyopaque) u16 {
    _ = a;
    _ = b;
    _ = c;
    out.* = null;
    return Err.invalid_state;
}

var dummy_arena: u8 = 0;

const plenty = fabric.Ops{ .remaining = remainingPlenty, .carve = carveRefuses };
const tiny = fabric.Ops{ .remaining = remainingTiny, .carve = carveRefuses };
const failing = fabric.Ops{ .remaining = remainingFails, .carve = carveRefuses };

test "an unbound handle is not initialised" {
    const dec = Handle{};
    var caps: Caps = .{};
    try std.testing.expectEqual(Err.not_initialized, fabric.fetchCaps(@ptrCast(&dec), @ptrCast(&caps)));
}

test "a vtable with no caps entry is an unusable state" {
    const empty_iface = Iface{};
    const dec = Handle{ .iface = &empty_iface };
    var caps: Caps = .{};
    try std.testing.expectEqual(Err.invalid_state, fabric.fetchCaps(@ptrCast(&dec), @ptrCast(&caps)));
}

test "a capability record is read back whole" {
    var backend = plainBackend();
    backend.caps.scratch_bytes = 512;
    const dec = handleFor(&backend);
    var caps: Caps = .{};
    try std.testing.expectEqual(Err.ok, fabric.fetchCaps(@ptrCast(&dec), @ptrCast(&caps)));
    try std.testing.expectEqual(@as(u32, 512), caps.scratch_bytes);
    try std.testing.expectEqual(Format.png, caps.formats);
}

test "an empty or out-of-range capability record is refused" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    var caps: Caps = .{};

    backend.caps.formats = 0;
    try std.testing.expectEqual(Err.invalid_state, fabric.fetchCaps(@ptrCast(&dec), @ptrCast(&caps)));

    backend.caps = plainBackend().caps;
    backend.caps.pixels = 0;
    try std.testing.expectEqual(Err.invalid_state, fabric.fetchCaps(@ptrCast(&dec), @ptrCast(&caps)));

    backend.caps = plainBackend().caps;
    backend.caps.dim_max = 0;
    try std.testing.expectEqual(Err.invalid_state, fabric.fetchCaps(@ptrCast(&dec), @ptrCast(&caps)));

    backend.caps = plainBackend().caps;
    backend.caps.dim_max = 16385;
    try std.testing.expectEqual(Err.invalid_state, fabric.fetchCaps(@ptrCast(&dec), @ptrCast(&caps)));
}

test "a bit outside the defined mask is an unusable record" {
    var backend = plainBackend();
    backend.caps.formats = 1 << 6;
    const dec = handleFor(&backend);
    var caps: Caps = .{};
    try std.testing.expectEqual(Err.invalid_state, fabric.fetchCaps(@ptrCast(&dec), @ptrCast(&caps)));
}

test "a backend's own error code travels back untranslated" {
    var backend = plainBackend();
    backend.caps_err = 0x4242;
    const dec = handleFor(&backend);
    var caps: Caps = .{};
    try std.testing.expectEqual(@as(u16, 0x4242), fabric.fetchCaps(@ptrCast(&dec), @ptrCast(&caps)));
}

test "supports answers only for a pair the backend advertised" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    var ok = true;

    try std.testing.expectEqual(Err.ok, fabric.supports(@ptrCast(&dec), Format.png, Pixel.rgba8888, &ok));
    try std.testing.expect(ok);

    try std.testing.expectEqual(Err.ok, fabric.supports(@ptrCast(&dec), Format.gif, Pixel.rgba8888, &ok));
    try std.testing.expect(!ok);

    try std.testing.expectEqual(Err.ok, fabric.supports(@ptrCast(&dec), Format.png, Pixel.grey8, &ok));
    try std.testing.expect(!ok);
}

test "supports needs exactly one defined bit on each side" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    var ok = true;

    try std.testing.expectEqual(Err.invalid_arg, fabric.supports(@ptrCast(&dec), 0, Pixel.rgba8888, &ok));
    try std.testing.expect(!ok);
    try std.testing.expectEqual(Err.invalid_arg, fabric.supports(@ptrCast(&dec), Format.png | Format.gif, Pixel.rgba8888, &ok));
    try std.testing.expectEqual(Err.invalid_arg, fabric.supports(@ptrCast(&dec), Format.png, 0, &ok));
    try std.testing.expectEqual(Err.invalid_arg, fabric.supports(@ptrCast(&dec), Format.png, 1 << 3, &ok));
}

fn reqFor(bytes: []const u8, dst: []u8) Req {
    return .{
        .bytes = bytes.ptr,
        .byte_count = @intCast(bytes.len),
        .dst = dst.ptr,
        .dst_bytes = @intCast(dst.len),
        .want = Pixel.rgba8888,
    };
}

test "a declared format the backend opens reaches it unchanged" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;

    var req = reqFor(&bytes, &dst);
    req.format = Format.png;
    var out: Image = .{};
    try std.testing.expectEqual(Err.ok, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));
    try std.testing.expectEqual(@as(u32, 1), backend.decode_calls);
    try std.testing.expectEqual(Format.png, backend.saw_format);
}

test "format none is sniffed and the backend is handed the resolved bit" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;

    var req = reqFor(&bytes, &dst);
    var out: Image = .{};
    try std.testing.expectEqual(Err.ok, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));
    try std.testing.expectEqual(Format.png, backend.saw_format);
}

test "bytes carrying no signature are unsupported, never handed on" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    const bytes = [_]u8{ 1, 2, 3, 4 };
    var dst = [_]u8{0} ** 64;

    var req = reqFor(&bytes, &dst);
    var out: Image = .{};
    try std.testing.expectEqual(Err.not_supported, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));
    try std.testing.expectEqual(@as(u32, 0), backend.decode_calls);
}

test "a format the backend did not advertise never reaches it" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    var bytes = [_]u8{0} ** 10;
    @memcpy(bytes[0..6], "GIF89a");
    var dst = [_]u8{0} ** 64;

    var req = reqFor(&bytes, &dst);
    var out: Image = .{};
    try std.testing.expectEqual(Err.not_supported, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));
    try std.testing.expectEqual(@as(u32, 0), backend.decode_calls);
}

test "a pixel layout the backend cannot write never reaches it" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;

    var req = reqFor(&bytes, &dst);
    req.want = Pixel.grey8;
    var out: Image = .{};
    try std.testing.expectEqual(Err.not_supported, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));
    try std.testing.expectEqual(@as(u32, 0), backend.decode_calls);
}

test "the request contract is checked before any dispatch" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;
    var out: Image = .{};

    var req = reqFor(&bytes, &dst);
    req.bytes = null;
    try std.testing.expectEqual(Err.null_ptr, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));

    req = reqFor(&bytes, &dst);
    req.byte_count = 0;
    try std.testing.expectEqual(Err.invalid_size, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));

    req = reqFor(&bytes, &dst);
    req.want = 0;
    try std.testing.expectEqual(Err.invalid_arg, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));

    req = reqFor(&bytes, &dst);
    req.format = Format.png | Format.gif;
    try std.testing.expectEqual(Err.invalid_arg, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));

    try std.testing.expectEqual(@as(u32, 0), backend.decode_calls);
}

test "a stride or destination too small for one pixel is a size fault" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;
    var out: Image = .{};

    var req = reqFor(&bytes, &dst);
    req.dst_stride = 3;
    try std.testing.expectEqual(Err.invalid_size, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));

    req = reqFor(&bytes, dst[0..2]);
    try std.testing.expectEqual(Err.invalid_size, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));
}

test "a backend that needs scratch is refused a request with no arena" {
    var backend = plainBackend();
    backend.caps.scratch_bytes = 1024;
    const dec = handleFor(&backend);
    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;

    var req = reqFor(&bytes, &dst);
    var out: Image = .{};
    try std.testing.expectEqual(Err.invalid_state, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));
}

test "an arena too small for the advertised scratch is out of memory" {
    var backend = plainBackend();
    backend.caps.scratch_bytes = 1024;
    const dec = handleFor(&backend);
    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;

    var req = reqFor(&bytes, &dst);
    req.arena = &dummy_arena;
    var out: Image = .{};
    try std.testing.expectEqual(Err.no_mem, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), tiny));
}

test "the arena's own error code travels back untranslated" {
    var backend = plainBackend();
    backend.caps.scratch_bytes = 16;
    const dec = handleFor(&backend);
    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;

    var req = reqFor(&bytes, &dst);
    req.arena = &dummy_arena;
    var out: Image = .{};
    try std.testing.expectEqual(@as(u16, 0x4343), fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), failing));
}

test "a backend that needs no scratch is never asked about the arena" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;

    arena_seen = 0;
    var req = reqFor(&bytes, &dst);
    var out: Image = .{};
    _ = fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty);
    try std.testing.expectEqual(@as(u32, 0), arena_seen);
}

test "a failed decode leaves the result zeroed, not half written" {
    var backend = plainBackend();
    backend.decode_err = Err.invalid_state;
    backend.produced = .{ .width_px = 99, .height_px = 99 };
    const dec = handleFor(&backend);
    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;

    var req = reqFor(&bytes, &dst);
    var out: Image = .{};
    try std.testing.expectEqual(Err.invalid_state, fabric.decode(@ptrCast(&dec), @ptrCast(&req), @ptrCast(&out), plenty));
    try std.testing.expectEqual(@as(u32, 0), out.width_px);
}

test "probe answers geometry for a container the backend opens" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    const bytes = pngOf(320, 200);

    var geom: Geom = .{};
    try std.testing.expectEqual(Err.ok, fabric.probe(@ptrCast(&dec), &bytes, @ptrCast(&geom)));
    try std.testing.expectEqual(@as(u32, 320), geom.width_px);
    try std.testing.expectEqual(Format.png, geom.format);
}

test "probe reports unrecognised bytes the way decode does, not the way dims does" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    var geom: Geom = .{};
    try std.testing.expectEqual(Err.not_supported, fabric.probe(@ptrCast(&dec), &[_]u8{ 1, 2, 3 }, @ptrCast(&geom)));
}

test "probe enforces the backend's own dim_max, which decode cannot" {
    var backend = plainBackend();
    backend.caps.dim_max = 256;
    const dec = handleFor(&backend);
    const bytes = pngOf(320, 200);

    var geom: Geom = .{};
    try std.testing.expectEqual(Err.not_supported, fabric.probe(@ptrCast(&dec), &bytes, @ptrCast(&geom)));
    try std.testing.expectEqual(@as(u32, 0), geom.width_px);
}

test "probe refuses a container this backend does not advertise" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    var bytes = [_]u8{0} ** 10;
    @memcpy(bytes[0..6], "GIF89a");
    bytes[6] = 4;
    bytes[8] = 4;

    var geom: Geom = .{};
    try std.testing.expectEqual(Err.not_supported, fabric.probe(@ptrCast(&dec), &bytes, @ptrCast(&geom)));
}

test "a declared dimension out of range keeps the dims size fault" {
    var backend = plainBackend();
    const dec = handleFor(&backend);
    const bytes = pngOf(0, 200);

    var geom: Geom = .{};
    try std.testing.expectEqual(Err.invalid_size, fabric.probe(@ptrCast(&dec), &bytes, @ptrCast(&geom)));
}
