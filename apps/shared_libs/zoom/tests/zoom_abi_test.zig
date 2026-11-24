// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Host C-ABI membrane tests for `src/zoom_abi.zig`.

const std = @import("std");
const abi = @import("abi");
const stubs = @import("zoom_stubs.zig");

comptime {
    _ = @import("zoom_stubs.zig");
    _ = stubs.LogSink();
    _ = stubs.BookStubs(abi.book);
    _ = stubs.DitherBlitStubs(abi.render);
    _ = stubs.ChromeStubs(abi.zoom.Err);
    _ = stubs.TileStubs(abi.tiles);
}

const Err = abi.zoom.Err;
const View = abi.zoom.View;
const Source = abi.zoom.Source;
const Cfg = abi.zoom.Cfg;
const Rect = abi.zoom.Rect;
const Present = abi.zoom.Present;
const Axis = abi.zoom.Axis;

fn nullRead(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: [*]u8, out_stride: u32) callconv(.c) Err {
    _ = ctx;
    _ = x;
    _ = y;
    _ = w;
    _ = h;
    _ = out;
    _ = out_stride;
    return .ok;
}

test "abi exports carry the C guards" {
    var v = View{};
    try std.testing.expectEqual(Err.null_ptr, abi.zoom_view_open(null, null));
    var cfg = Cfg{ .src = .{ .read = nullRead, .width = 16, .height = 16 }, .dst = .{ .w = 8, .h = 8 } };
    var scratch: [16]u8 = undefined;
    cfg.scratch = .{ .row = scratch[0..].ptr, .strip = scratch[0..].ptr, .packed_buf = scratch[0..].ptr, .row_cap = 8, .strip_cap = 8, .packed_cap = 4 };
    try std.testing.expectEqual(Err.ok, abi.zoom_view_open(&v, &cfg));
    try std.testing.expect(abi.zoom_view_active(&v));
    try std.testing.expectEqual(Err.ok, abi.zoom_view_close(&v));
    try std.testing.expect(!abi.zoom_view_active(&v));
    try std.testing.expectEqual(Err.null_ptr, abi.zoom_view_rebind(null, null, 0));
    try std.testing.expectEqual(Err.null_ptr, abi.zoom_view_invalidate(null, 0));
    try std.testing.expectEqual(Err.null_ptr, abi.zoom_view_set_scale(null, 1, 0, 0, 0));
    try std.testing.expectEqual(Err.null_ptr, abi.zoom_view_pan(null, 0, 0, 0));
    try std.testing.expectEqual(Err.null_ptr, abi.zoom_view_pan_dir(null, .none, 0));
    var win = Rect{};
    try std.testing.expectEqual(Err.null_ptr, abi.zoom_view_window(null, &win));
    try std.testing.expect(!abi.zoom_view_tick(null, 0));
    var pres = Present{};
    try std.testing.expectEqual(Err.null_ptr, abi.zoom_view_present(null, &pres));
}

test "priv_zoom_axis resolves one axis like the C did" {
    var ax = Axis{};
    abi.priv_zoom_axis(0, 32, 4, 16, &ax);
    try std.testing.expectEqual(@as(i32, 0), ax.d0);
    try std.testing.expectEqual(@as(i32, 32), ax.d1);
    try std.testing.expectEqual(@as(u32, 0), ax.s0);
    try std.testing.expectEqual(@as(u32, 8), ax.count);
    abi.priv_zoom_axis(100, 32, 2, 8, &ax);
    try std.testing.expectEqual(@as(i32, 0), ax.d0);
    try std.testing.expectEqual(@as(i32, 0), ax.d1);
    try std.testing.expectEqual(@as(u32, 0), ax.count);
}

test "zoom_mem and zoom_popup C ABI exports carry C guards and state" {
    const px = [_]u8{ 0x10, 0x20, 0x30, 0x40 };
    var ms = abi.mem.MemSrc{};
    try std.testing.expectEqual(Err.ok, abi.zoom_mem_src_init(&ms, &px, 2, 2, 0));
    var src = Source{};
    try std.testing.expectEqual(Err.ok, abi.zoom_mem_src_bind(&ms, &src));
    var out: [4]u8 = undefined;
    try std.testing.expectEqual(Err.ok, abi.zoom_mem_read(&ms, 0, 0, 2, 2, &out, 2));
    try std.testing.expectEqual([4]u8{ 0x10, 0x20, 0x30, 0x40 }, out);

    var p = abi.popup.Popup{};
    try std.testing.expect(!abi.zoom_popup_active(&p));
    try std.testing.expect(!abi.zoom_popup_tick(&p, 0));
    try std.testing.expectEqual(abi.popup.PopupZone.none, abi.zoom_popup_zone_hit(&p, 0, 0));
    var scratch: [128]u8 = undefined;
    const cfg = abi.popup.PopupCfg{
        .src = src,
        .scratch = .{ .row = &scratch, .row_cap = 64, .strip = &scratch, .strip_cap = 128, .packed_buf = &scratch, .packed_cap = 64 },
        .fb_w = 64,
        .fb_h = 64,
    };
    try std.testing.expectEqual(Err.ok, abi.zoom_popup_open(&p, &cfg));
    try std.testing.expect(abi.zoom_popup_active(&p));
    var act: abi.popup.PopupAction = .none;
    try std.testing.expectEqual(Err.ok, abi.zoom_popup_tap(&p, 2, 2, 10, &act));
    try std.testing.expectEqual(abi.popup.PopupAction.closed, act);
    try std.testing.expect(!abi.zoom_popup_active(&p));
}
