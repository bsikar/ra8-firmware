//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A set of decoders answered as one: which member a format routes to, what
//! the set as a whole can open, and the scratch budget that covers all of it.

const std = @import("std");
const mux = @import("mux");

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

const plenty = mux.Ops{ .remaining = remainingPlenty, .carve = carveRefuses };

const Mux = extern struct {
    members: [4]Handle = @splat(.{}),
    count: u32 = 0,
};

fn backendOf(formats: u32, pixels: u32) Backend {
    return .{ .caps = .{ .formats = formats, .pixels = pixels, .dim_max = 4096 } };
}

test "an empty set is not initialised" {
    var set = Mux{};
    var ok = true;
    try std.testing.expectEqual(Err.not_initialized, mux.supports(@ptrCast(&set), Format.png, Pixel.rgba8888, &ok));
    try std.testing.expect(!ok);
}

test "init clears the set" {
    var set = Mux{ .count = 3 };
    try std.testing.expectEqual(Err.ok, mux.init(@ptrCast(&set)));
    try std.testing.expectEqual(@as(u32, 0), set.count);
}

test "add takes a copy once the capability record reads back" {
    var backend = backendOf(Format.png, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    try std.testing.expectEqual(Err.ok, mux.add(@ptrCast(&set), @ptrCast(&dec)));
    try std.testing.expectEqual(@as(u32, 1), set.count);
}

test "add refuses a backend whose record is unusable" {
    var backend = backendOf(0, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    try std.testing.expectEqual(Err.invalid_state, mux.add(@ptrCast(&set), @ptrCast(&dec)));
    try std.testing.expectEqual(@as(u32, 0), set.count);
}

test "the set holds four members and refuses a fifth" {
    var backend = backendOf(Format.png, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    for (0..4) |_| try std.testing.expectEqual(Err.ok, mux.add(@ptrCast(&set), @ptrCast(&dec)));
    try std.testing.expectEqual(Err.no_mem, mux.add(@ptrCast(&set), @ptrCast(&dec)));
}

test "formats unions every member that writes the layout asked for" {
    var png_rgba = backendOf(Format.png, Pixel.rgba8888);
    var gif_grey = backendOf(Format.gif, Pixel.grey8);
    var webp_rgba = backendOf(Format.webp, Pixel.rgba8888);
    const a = handleFor(&png_rgba);
    const b = handleFor(&gif_grey);
    const c = handleFor(&webp_rgba);

    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&a));
    _ = mux.add(@ptrCast(&set), @ptrCast(&b));
    _ = mux.add(@ptrCast(&set), @ptrCast(&c));

    var openable: u32 = 0;
    try std.testing.expectEqual(Err.ok, mux.formats(@ptrCast(&set), Pixel.rgba8888, &openable));
    try std.testing.expectEqual(Format.png | Format.webp, openable);

    try std.testing.expectEqual(Err.ok, mux.formats(@ptrCast(&set), Pixel.grey8, &openable));
    try std.testing.expectEqual(Format.gif, openable);
}

test "formats needs one defined pixel bit" {
    var backend = backendOf(Format.png, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&dec));

    var openable: u32 = 0;
    try std.testing.expectEqual(Err.invalid_arg, mux.formats(@ptrCast(&set), 0, &openable));
    try std.testing.expectEqual(Err.invalid_arg, mux.formats(@ptrCast(&set), Pixel.grey8 | Pixel.rgb888, &openable));
}

test "route picks the first member covering both sides" {
    var first = backendOf(Format.png, Pixel.rgba8888);
    var second = backendOf(Format.png | Format.gif, Pixel.rgba8888);
    const a = handleFor(&first);
    const b = handleFor(&second);

    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&a));
    _ = mux.add(@ptrCast(&set), @ptrCast(&b));

    var member: ?*const Handle = null;
    try std.testing.expectEqual(Err.ok, mux.route(@ptrCast(&set), Format.png, Pixel.rgba8888, @ptrCast(&member)));
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&first)), member.?.ctx);

    try std.testing.expectEqual(Err.ok, mux.route(@ptrCast(&set), Format.gif, Pixel.rgba8888, @ptrCast(&member)));
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&second)), member.?.ctx);
}

test "a pair nothing in the set covers is unsupported" {
    var backend = backendOf(Format.png, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&dec));

    var member: ?*const Handle = null;
    try std.testing.expectEqual(Err.not_supported, mux.route(@ptrCast(&set), Format.bmp, Pixel.rgba8888, @ptrCast(&member)));
    try std.testing.expect(member == null);
}

test "supports is route without the refusal" {
    var backend = backendOf(Format.png, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&dec));

    var ok = false;
    try std.testing.expectEqual(Err.ok, mux.supports(@ptrCast(&set), Format.png, Pixel.rgba8888, &ok));
    try std.testing.expect(ok);
    try std.testing.expectEqual(Err.ok, mux.supports(@ptrCast(&set), Format.bmp, Pixel.rgba8888, &ok));
    try std.testing.expect(!ok);
}

test "decode sniffs, routes, and hands the member a resolved format" {
    var png_rgba = backendOf(Format.png, Pixel.rgba8888);
    var gif_rgba = backendOf(Format.gif, Pixel.rgba8888);
    const a = handleFor(&png_rgba);
    const b = handleFor(&gif_rgba);

    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&a));
    _ = mux.add(@ptrCast(&set), @ptrCast(&b));

    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;
    var req = Req{
        .bytes = (&bytes).ptr,
        .byte_count = bytes.len,
        .dst = (&dst).ptr,
        .dst_bytes = dst.len,
        .want = Pixel.rgba8888,
    };
    var out: Image = .{};

    try std.testing.expectEqual(Err.ok, mux.decode(@ptrCast(&set), @ptrCast(&req), @ptrCast(&out), plenty));
    try std.testing.expectEqual(@as(u32, 1), png_rgba.decode_calls);
    try std.testing.expectEqual(@as(u32, 0), gif_rgba.decode_calls);
    try std.testing.expectEqual(Format.png, png_rgba.saw_format);
}

test "decode refuses bytes nothing in the set opens" {
    var backend = backendOf(Format.gif, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&dec));

    const bytes = pngOf(16, 16);
    var dst = [_]u8{0} ** 64;
    var req = Req{
        .bytes = (&bytes).ptr,
        .byte_count = bytes.len,
        .dst = (&dst).ptr,
        .dst_bytes = dst.len,
        .want = Pixel.rgba8888,
    };
    var out: Image = .{};

    try std.testing.expectEqual(Err.not_supported, mux.decode(@ptrCast(&set), @ptrCast(&req), @ptrCast(&out), plenty));
    try std.testing.expectEqual(@as(u32, 0), backend.decode_calls);
}

test "the scratch budget is the widest demand and the strictest alignment" {
    var small = backendOf(Format.png, Pixel.rgba8888);
    small.caps.scratch_bytes = 256;
    small.caps.scratch_align = 4;
    var large = backendOf(Format.gif, Pixel.rgba8888);
    large.caps.scratch_bytes = 4096;
    large.caps.scratch_align = 16;
    const a = handleFor(&small);
    const b = handleFor(&large);

    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&a));
    _ = mux.add(@ptrCast(&set), @ptrCast(&b));

    var bytes: u32 = 0;
    var alignment: u32 = 0;
    try std.testing.expectEqual(Err.ok, mux.scratchBudget(@ptrCast(&set), &bytes, &alignment));
    try std.testing.expectEqual(@as(u32, 4096), bytes);
    try std.testing.expectEqual(@as(u32, 16), alignment);
}

test "a set where nothing needs scratch budgets nothing" {
    var backend = backendOf(Format.png, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&dec));

    var bytes: u32 = 0;
    var alignment: u32 = 0;
    try std.testing.expectEqual(Err.ok, mux.scratchBudget(@ptrCast(&set), &bytes, &alignment));
    try std.testing.expectEqual(@as(u32, 0), bytes);
}

test "probe names the geometry and the member that would take it" {
    var backend = backendOf(Format.png, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&dec));

    const bytes = pngOf(320, 200);
    var geom: Geom = .{};
    var member: ?*const Handle = null;
    try std.testing.expectEqual(Err.ok, mux.probe(@ptrCast(&set), &bytes, Pixel.rgba8888, @ptrCast(&geom), @ptrCast(&member)));
    try std.testing.expectEqual(@as(u32, 320), geom.width_px);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&backend)), member.?.ctx);
}

test "probe leaves the member slot empty on a refusal" {
    var backend = backendOf(Format.gif, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&dec));

    const bytes = pngOf(320, 200);
    var geom: Geom = .{};
    var member: ?*const Handle = null;
    try std.testing.expectEqual(Err.not_supported, mux.probe(@ptrCast(&set), &bytes, Pixel.rgba8888, @ptrCast(&geom), @ptrCast(&member)));
    try std.testing.expect(member == null);
    try std.testing.expectEqual(@as(u32, 0), geom.width_px);
}

test "probe of an empty buffer is a size fault" {
    var backend = backendOf(Format.png, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&dec));

    var geom: Geom = .{};
    try std.testing.expectEqual(Err.invalid_size, mux.probe(@ptrCast(&set), &[_]u8{}, Pixel.rgba8888, @ptrCast(&geom), null));
}

test "probe takes a null member slot" {
    var backend = backendOf(Format.png, Pixel.rgba8888);
    const dec = handleFor(&backend);
    var set = Mux{};
    _ = mux.add(@ptrCast(&set), @ptrCast(&dec));

    const bytes = pngOf(64, 64);
    var geom: Geom = .{};
    try std.testing.expectEqual(Err.ok, mux.probe(@ptrCast(&set), &bytes, Pixel.rgba8888, @ptrCast(&geom), null));
    try std.testing.expectEqual(@as(u32, 64), geom.height_px);
}
