// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the core zoom viewport state machine (`src/zoom.zig`).

const std = @import("std");
const zoom = @import("zoom");
const stubs = @import("zoom_stubs.zig");

comptime {
    _ = @import("zoom_stubs.zig");
    _ = stubs.LogSink();
}

const Err = zoom.Err;
const Rect = zoom.Rect;
const Source = zoom.Source;
const Scratch = zoom.Scratch;
const View = zoom.View;
const Cfg = zoom.Cfg;
const Present = zoom.Present;
const Refresh = zoom.Refresh;

fn testRead(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: [*]u8, out_stride: u32) callconv(.c) Err {
    _ = ctx;
    _ = x;
    _ = y;
    _ = w;
    _ = h;
    _ = out;
    _ = out_stride;
    return .ok;
}

test "axis clips the plane to the viewport and names the source span" {
    const a = zoom.axis(0, 32, 4, 16);
    try std.testing.expectEqual(@as(i32, 0), a.d0);
    try std.testing.expectEqual(@as(i32, 32), a.d1);
    try std.testing.expectEqual(@as(u32, 0), a.s0);
    try std.testing.expectEqual(@as(u32, 8), a.count);
}

test "axis past the right edge clamps to a zero-count window" {
    const a = zoom.axis(64, 32, 4, 16);
    try std.testing.expectEqual(@as(u32, 0), a.count);
}

test "axis on a mid-page anchor keeps the overlap window" {
    const a = zoom.axis(32, 32, 4, 16);
    try std.testing.expectEqual(@as(i32, 0), a.d0);
    try std.testing.expectEqual(@as(i32, 32), a.d1);
    try std.testing.expectEqual(@as(u32, 8), a.s0);
    try std.testing.expectEqual(@as(u32, 8), a.count);
}

test "dimOk bounds" {
    try std.testing.expect(!zoom.dimOk(0));
    try std.testing.expect(zoom.dimOk(1));
    try std.testing.expect(zoom.dimOk(zoom.dim_max));
    try std.testing.expect(!zoom.dimOk(zoom.dim_max + 1));
}

test "satAdd clamps at the int32 boundary instead of wrapping" {
    try std.testing.expectEqual(@as(i32, 3), zoom.satAdd(1, 2));
    try std.testing.expectEqual(std.math.maxInt(i32), zoom.satAdd(std.math.maxInt(i32), 1));
    try std.testing.expectEqual(std.math.minInt(i32), zoom.satAdd(std.math.minInt(i32), -1));
}

test "clampAnchor centres a small image and bounds a big one" {
    try std.testing.expectEqual(@as(i32, -4), zoom.clampAnchor(0, 24, 32));
    try std.testing.expectEqual(@as(i32, 0), zoom.clampAnchor(-9, 64, 32));
    try std.testing.expectEqual(@as(i32, 32), zoom.clampAnchor(99, 64, 32));
    try std.testing.expectEqual(@as(i32, 10), zoom.clampAnchor(10, 64, 32));
}

test "reclamp bounds a big image and centres a small one" {
    var v = View{ .src = .{ .width = 16, .height = 16 }, .dst = .{ .w = 32, .h = 32 }, .scale = 4 };
    v.anchor_x = 99;
    v.anchor_y = -9;
    zoom.reclamp(&v);
    try std.testing.expectEqual(@as(i32, 32), v.anchor_x);
    try std.testing.expectEqual(@as(i32, 0), v.anchor_y);
    var s = View{ .src = .{ .width = 8, .height = 8 }, .dst = .{ .w = 32, .h = 32 }, .scale = 2 };
    s.anchor_x = 5;
    s.anchor_y = 5;
    zoom.reclamp(&s);
    try std.testing.expectEqual(@as(i32, -8), s.anchor_x);
    try std.testing.expectEqual(@as(i32, -8), s.anchor_y);
}

test "touch under responsive arms settle and owes fast" {
    var v = View{ .policy = .responsive };
    zoom.touch(&v, 1000);
    try std.testing.expectEqual(@as(u32, 1000), v.last_input_ms);
    try std.testing.expect(v.pending);
    try std.testing.expectEqual(Refresh.fast, v.pending_kind);
    try std.testing.expect(v.settle_armed);
}

test "touch under quality owes a full flush and arms nothing" {
    var v = View{ .policy = .quality };
    zoom.touch(&v, 1000);
    try std.testing.expect(v.pending);
    try std.testing.expectEqual(Refresh.quality, v.pending_kind);
    try std.testing.expect(!v.settle_armed);
}

test "close drops everything the view owes" {
    var v = View{ .active = true, .pending = true, .settle_armed = true };
    try std.testing.expect(zoom.isActive(&v));
    zoom.close(&v);
    try std.testing.expect(!zoom.isActive(&v));
    try std.testing.expect(!v.pending);
    try std.testing.expect(!v.settle_armed);
}

test "cfgPtrsOk rejects each missing pointer" {
    var buf: [64]u8 = undefined;
    var cfg = Cfg{ .src = .{ .read = testRead, .width = 16, .height = 16 }, .dst = .{ .w = 32, .h = 24 } };
    cfg.scratch = .{ .row = buf[0..].ptr, .strip = buf[0..].ptr, .packed_buf = buf[0..].ptr, .row_cap = 32, .strip_cap = 32, .packed_cap = 16 };
    try std.testing.expectEqual(Err.ok, zoom.cfgPtrsOk(&cfg));
    cfg.src.read = null;
    try std.testing.expectEqual(Err.null_ptr, zoom.cfgPtrsOk(&cfg));
    cfg.src.read = testRead;
    cfg.scratch.row = null;
    try std.testing.expectEqual(Err.null_ptr, zoom.cfgPtrsOk(&cfg));
}

test "cfgExtentOk checks viewport and source extents" {
    var buf: [64]u8 = undefined;
    var cfg = Cfg{ .src = .{ .read = testRead, .width = 640, .height = 480 }, .dst = .{ .w = 32, .h = 24 } };
    cfg.scratch = .{ .row = buf[0..].ptr, .strip = buf[0..].ptr, .packed_buf = buf[0..].ptr, .row_cap = 32, .strip_cap = 32, .packed_cap = 16 };
    try std.testing.expectEqual(Err.ok, zoom.cfgExtentOk(&cfg));
    cfg.dst.w = 0;
    try std.testing.expectEqual(Err.invalid_arg, zoom.cfgExtentOk(&cfg));
    cfg.dst.w = 32;
    cfg.src.width = zoom.dim_max + 1;
    try std.testing.expectEqual(Err.invalid_arg, zoom.cfgExtentOk(&cfg));
}

test "cfgScratchOk derives strip rows and caps the packed need" {
    var buf: [64]u8 = undefined;
    var cfg = Cfg{ .src = .{ .read = testRead, .width = 640, .height = 480 }, .dst = .{ .w = 32, .h = 2 } };
    cfg.scratch = .{ .row = buf[0..].ptr, .strip = buf[0..].ptr, .packed_buf = buf[0..].ptr, .row_cap = 32, .strip_cap = 64, .packed_cap = 33 };
    var rows: u16 = 0;
    try std.testing.expectEqual(Err.ok, zoom.cfgScratchOk(&cfg, &rows));
    try std.testing.expectEqual(@as(u16, 2), rows);
    cfg.dst.h = 40;
    cfg.scratch.strip_cap = 32 * 40;
    cfg.scratch.packed_cap = 512;
    try std.testing.expectEqual(Err.ok, zoom.cfgScratchOk(&cfg, &rows));
    try std.testing.expectEqual(@as(u16, 32), rows);
    cfg.scratch.row_cap = 31;
    try std.testing.expectEqual(Err.no_mem, zoom.cfgScratchOk(&cfg, &rows));
    cfg.scratch.row_cap = 32;
    cfg.scratch.packed_cap = 511;
    try std.testing.expectEqual(Err.no_mem, zoom.cfgScratchOk(&cfg, &rows));
}

test "cfgLadderOk lifts zero defaults and bounds the ladder" {
    var cfg = Cfg{ .src = .{ .read = undefined, .width = 16, .height = 16 }, .dst = .{ .w = 32, .h = 24 } };
    var opening: u8 = 0;
    var ceiling: u8 = 0;
    cfg.scale = 0;
    cfg.scale_max = 0;
    try std.testing.expectEqual(Err.ok, zoom.cfgLadderOk(&cfg, &opening, &ceiling));
    try std.testing.expectEqual(@as(u8, 1), opening);
    try std.testing.expectEqual(@as(u8, 8), ceiling);
    cfg.scale = 3;
    cfg.scale_max = 4;
    try std.testing.expectEqual(Err.ok, zoom.cfgLadderOk(&cfg, &opening, &ceiling));
    try std.testing.expectEqual(@as(u8, 3), opening);
    try std.testing.expectEqual(@as(u8, 4), ceiling);
    cfg.scale_max = 9;
    try std.testing.expectEqual(Err.invalid_arg, zoom.cfgLadderOk(&cfg, &opening, &ceiling));
    cfg.scale_max = 4;
    cfg.scale = 5;
    try std.testing.expectEqual(Err.invalid_arg, zoom.cfgLadderOk(&cfg, &opening, &ceiling));
}

test "sourceInit binds reader, context and extent" {
    var src = Source{};
    try std.testing.expectEqual(Err.ok, zoom.sourceInit(&src, testRead, null, 800, 600));
    try std.testing.expectEqual(@as(u32, 800), src.width);
    try std.testing.expectEqual(@as(u32, 600), src.height);
    try std.testing.expectEqual(Err.null_ptr, zoom.sourceInit(null, testRead, null, 8, 8));
    try std.testing.expectEqual(Err.null_ptr, zoom.sourceInit(&src, null, null, 8, 8));
    try std.testing.expectEqual(Err.invalid_arg, zoom.sourceInit(&src, testRead, null, 0, 8));
}

test "log hook receives rejected-config messages" {
    const Hook = struct {
        var hits_g: usize = 0;
        fn f(_: []const u8) void {
            hits_g += 1;
        }
    };
    Hook.hits_g = 0;
    zoom.log_hook = Hook.f;
    defer zoom.log_hook = null;
    var cfg = Cfg{ .src = .{ .width = 16, .height = 16 }, .dst = .{ .w = 32, .h = 24 } };
    cfg.scratch = .{ .row_cap = 32, .strip_cap = 32, .packed_cap = 16 };
    try std.testing.expectEqual(Err.null_ptr, zoom.cfgPtrsOk(&cfg));
    try std.testing.expectEqual(@as(usize, 1), Hook.hits_g);
}

test "viewOpen adopts a valid config" {
    var buf: [64]u8 = undefined;
    var cfg = Cfg{ .src = .{ .read = testRead, .width = 640, .height = 480 }, .dst = .{ .w = 32, .h = 24 }, .focus_x = 10, .focus_y = 8, .scale = 2, .scale_max = 4 };
    cfg.scratch = .{ .row = buf[0..].ptr, .strip = buf[0..].ptr, .packed_buf = buf[0..].ptr, .row_cap = 32, .strip_cap = 32, .packed_cap = 24 };
    var v = View{};
    try std.testing.expectEqual(Err.ok, zoom.viewOpen(&v, &cfg));
    try std.testing.expect(v.active);
    try std.testing.expectEqual(@as(u8, 2), v.scale);
    try std.testing.expectEqual(@as(u8, 4), v.scale_max);
    try std.testing.expectEqual(@as(u16, 350), v.settle_ms);
    try std.testing.expect(v.pending);
    try std.testing.expectEqual(@as(i32, 4), v.anchor_x);
    try std.testing.expectEqual(@as(i32, 4), v.anchor_y);
    var closed = View{};
    cfg.dst.w = 0;
    try std.testing.expectEqual(Err.invalid_arg, zoom.viewOpen(&closed, &cfg));
    try std.testing.expect(!closed.active);
}

test "viewRebind guards state and re-clamps to the new source" {
    var buf: [64]u8 = undefined;
    var cfg = Cfg{ .src = .{ .read = testRead, .width = 640, .height = 480 }, .dst = .{ .w = 32, .h = 24 } };
    cfg.scratch = .{ .row = buf[0..].ptr, .strip = buf[0..].ptr, .packed_buf = buf[0..].ptr, .row_cap = 32, .strip_cap = 32, .packed_cap = 16 };
    var v = View{};
    try std.testing.expectEqual(Err.ok, zoom.viewOpen(&v, &cfg));
    var src2 = Source{ .read = testRead, .width = 100, .height = 100 };
    try std.testing.expectEqual(Err.ok, zoom.viewRebind(&v, &src2, 500));
    try std.testing.expectEqual(@as(u32, 100), v.src.width);
    var closed = View{};
    try std.testing.expectEqual(Err.invalid_state, zoom.viewRebind(&closed, &src2, 10));
    try std.testing.expectEqual(Err.invalid_state, zoom.viewInvalidate(&closed, 10));
    try std.testing.expectEqual(Err.ok, zoom.viewInvalidate(&v, 900));
}

test "scaleCycle doubles and wraps" {
    try std.testing.expectEqual(@as(u8, 2), zoom.scaleCycle(1, 1, 8));
    try std.testing.expectEqual(@as(u8, 8), zoom.scaleCycle(4, 1, 8));
    try std.testing.expectEqual(@as(u8, 1), zoom.scaleCycle(8, 1, 8));
    try std.testing.expectEqual(@as(u8, 4), zoom.scaleCycle(2, 2, 4));
    try std.testing.expectEqual(@as(u8, 8), zoom.scaleCycle(5, 1, 8));
    try std.testing.expectEqual(@as(u8, 3), zoom.scaleCycle(1, 3, 8));
}

test "viewSetScale keeps the focus plane point fixed" {
    var buf: [64]u8 = undefined;
    var cfg = Cfg{ .src = .{ .read = testRead, .width = 640, .height = 480 }, .dst = .{ .w = 32, .h = 24 }, .focus_x = 10, .focus_y = 8, .scale = 2, .scale_max = 4 };
    cfg.scratch = .{ .row = buf[0..].ptr, .strip = buf[0..].ptr, .packed_buf = buf[0..].ptr, .row_cap = 32, .strip_cap = 32, .packed_cap = 24 };
    var v = View{};
    try std.testing.expectEqual(Err.ok, zoom.viewOpen(&v, &cfg));
    try std.testing.expectEqual(Err.ok, zoom.viewSetScale(&v, 4, 16, 12, 100));
    try std.testing.expectEqual(@as(u8, 4), v.scale);
    try std.testing.expectEqual(@as(i32, 24), v.anchor_x);
    try std.testing.expectEqual(@as(i32, 20), v.anchor_y);
    try std.testing.expectEqual(Err.ok, zoom.viewSetScale(&v, 4, 16, 12, 200));
    try std.testing.expectEqual(Err.out_of_range, zoom.viewSetScale(&v, 5, 0, 0, 300));
    var closed = View{};
    try std.testing.expectEqual(Err.invalid_state, zoom.viewSetScale(&closed, 2, 0, 0, 0));
}

test "viewPan touches only when it moves" {
    var buf: [64]u8 = undefined;
    var cfg = Cfg{ .src = .{ .read = testRead, .width = 640, .height = 480 }, .dst = .{ .w = 32, .h = 24 } };
    cfg.scratch = .{ .row = buf[0..].ptr, .strip = buf[0..].ptr, .packed_buf = buf[0..].ptr, .row_cap = 32, .strip_cap = 32, .packed_cap = 16 };
    var v = View{};
    try std.testing.expectEqual(Err.ok, zoom.viewOpen(&v, &cfg));
    try std.testing.expectEqual(Err.ok, zoom.viewPan(&v, 4, 0, 100));
    try std.testing.expectEqual(@as(i32, 4), v.anchor_x);
    try std.testing.expect(v.pending);
    v.pending = false;
    try std.testing.expectEqual(Err.ok, zoom.viewPan(&v, 0, 0, 200));
    try std.testing.expect(!v.pending);
    try std.testing.expectEqual(Err.ok, zoom.viewPanDir(&v, .right, 300));
    try std.testing.expectEqual(@as(i32, 32), v.anchor_x);
    var closed = View{};
    try std.testing.expectEqual(Err.invalid_state, zoom.viewPan(&closed, 1, 0, 0));
    try std.testing.expectEqual(Err.invalid_state, zoom.viewPanDir(&closed, .up, 0));
}

test "viewWindow names the clipped source span" {
    var buf: [64]u8 = undefined;
    var cfg = Cfg{ .src = .{ .read = testRead, .width = 640, .height = 480 }, .dst = .{ .w = 32, .h = 24 } };
    cfg.scratch = .{ .row = buf[0..].ptr, .strip = buf[0..].ptr, .packed_buf = buf[0..].ptr, .row_cap = 32, .strip_cap = 32, .packed_cap = 16 };
    var v = View{};
    try std.testing.expectEqual(Err.ok, zoom.viewOpen(&v, &cfg));
    var win = Rect{};
    try std.testing.expectEqual(Err.ok, zoom.viewWindow(&v, &win));
    try std.testing.expectEqual(@as(i32, 0), win.x);
    try std.testing.expectEqual(@as(i32, 0), win.y);
    try std.testing.expect(win.w > 0 and win.w <= 640);
    try std.testing.expect(win.h > 0 and win.h <= 480);
    var closed = View{};
    try std.testing.expectEqual(Err.invalid_state, zoom.viewWindow(&closed, &win));
    try std.testing.expectEqual(Err.null_ptr, zoom.viewWindow(&v, null));
}

test "viewTick promotes to quality after the settle window" {
    var buf: [64]u8 = undefined;
    var cfg = Cfg{ .src = .{ .read = testRead, .width = 640, .height = 480 }, .dst = .{ .w = 32, .h = 24 } };
    cfg.scratch = .{ .row = buf[0..].ptr, .strip = buf[0..].ptr, .packed_buf = buf[0..].ptr, .row_cap = 32, .strip_cap = 32, .packed_cap = 16 };
    var v = View{};
    try std.testing.expectEqual(Err.ok, zoom.viewOpen(&v, &cfg));
    try std.testing.expectEqual(Err.ok, zoom.viewPan(&v, 4, 0, 100));
    try std.testing.expect(!zoom.viewTick(&v, 100));
    try std.testing.expect(!zoom.viewTick(&v, 449));
    try std.testing.expect(zoom.viewTick(&v, 450));
    try std.testing.expect(!v.settle_armed);
    try std.testing.expect(v.pending);
    try std.testing.expectEqual(Refresh.quality, v.pending_kind);
    try std.testing.expect(!zoom.viewTick(&v, 500));
}

test "viewPresent clears the pending flag" {
    var buf: [64]u8 = undefined;
    var cfg = Cfg{ .src = .{ .read = testRead, .width = 640, .height = 480 }, .dst = .{ .w = 32, .h = 24 } };
    cfg.scratch = .{ .row = buf[0..].ptr, .strip = buf[0..].ptr, .packed_buf = buf[0..].ptr, .row_cap = 32, .strip_cap = 32, .packed_cap = 16 };
    var v = View{};
    try std.testing.expectEqual(Err.ok, zoom.viewOpen(&v, &cfg));
    try std.testing.expectEqual(Err.ok, zoom.viewPan(&v, 4, 0, 100));
    var pres = Present{};
    try std.testing.expectEqual(Err.ok, zoom.viewPresent(&v, &pres));
    try std.testing.expect(pres.present);
    try std.testing.expectEqual(Refresh.fast, pres.refresh);
    try std.testing.expectEqual(@as(i32, 32), pres.rect.w);
    try std.testing.expect(!v.pending);
    try std.testing.expectEqual(Err.ok, zoom.viewPresent(&v, &pres));
    try std.testing.expect(!pres.present);
    var closed = View{};
    try std.testing.expectEqual(Err.invalid_state, zoom.viewPresent(&closed, &pres));
}

test "zoom struct layout matches zoom.h" {
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(Rect));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(Rect, "x"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(Rect, "y"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(Rect, "w"));
    try std.testing.expectEqual(@as(usize, 12), @offsetOf(Rect, "h"));
    try std.testing.expectEqual(@as(usize, 24), @sizeOf(Source));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(Source, "read"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(Source, "ctx"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(Source, "width"));
    try std.testing.expectEqual(@as(usize, 20), @offsetOf(Source, "height"));
    try std.testing.expectEqual(@as(usize, 48), @sizeOf(Scratch));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(Scratch, "row"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(Scratch, "row_cap"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(Scratch, "strip"));
    try std.testing.expectEqual(@as(usize, 24), @offsetOf(Scratch, "strip_cap"));
    try std.testing.expectEqual(@as(usize, 32), @offsetOf(Scratch, "packed_buf"));
    try std.testing.expectEqual(@as(usize, 40), @offsetOf(Scratch, "packed_cap"));
    try std.testing.expectEqual(@as(usize, 120), @sizeOf(View));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(View, "src"));
    try std.testing.expectEqual(@as(usize, 24), @offsetOf(View, "scratch"));
    try std.testing.expectEqual(@as(usize, 72), @offsetOf(View, "dst"));
    try std.testing.expectEqual(@as(usize, 88), @offsetOf(View, "anchor_x"));
    try std.testing.expectEqual(@as(usize, 92), @offsetOf(View, "anchor_y"));
    try std.testing.expectEqual(@as(usize, 96), @offsetOf(View, "last_input_ms"));
    try std.testing.expectEqual(@as(usize, 100), @offsetOf(View, "settle_ms"));
    try std.testing.expectEqual(@as(usize, 102), @offsetOf(View, "strip_rows"));
    try std.testing.expectEqual(@as(usize, 104), @offsetOf(View, "scale"));
    try std.testing.expectEqual(@as(usize, 105), @offsetOf(View, "scale_max"));
    try std.testing.expectEqual(@as(usize, 106), @offsetOf(View, "policy"));
    try std.testing.expectEqual(@as(usize, 107), @offsetOf(View, "pending_kind"));
    try std.testing.expectEqual(@as(usize, 108), @offsetOf(View, "pending"));
    try std.testing.expectEqual(@as(usize, 109), @offsetOf(View, "settle_armed"));
    try std.testing.expectEqual(@as(usize, 110), @offsetOf(View, "active"));
    try std.testing.expectEqual(@as(usize, 112), @offsetOf(View, "tone"));
    try std.testing.expectEqual(@as(usize, 112), @sizeOf(Cfg));
    try std.testing.expectEqual(@as(usize, 88), @offsetOf(Cfg, "scale"));
    try std.testing.expectEqual(@as(usize, 89), @offsetOf(Cfg, "scale_max"));
    try std.testing.expectEqual(@as(usize, 90), @offsetOf(Cfg, "policy"));
    try std.testing.expectEqual(@as(usize, 92), @offsetOf(Cfg, "settle_ms"));
    try std.testing.expectEqual(@as(usize, 96), @offsetOf(Cfg, "focus_x"));
    try std.testing.expectEqual(@as(usize, 100), @offsetOf(Cfg, "focus_y"));
    try std.testing.expectEqual(@as(usize, 104), @offsetOf(Cfg, "tone"));
    try std.testing.expectEqual(@as(usize, 20), @sizeOf(Present));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(Present, "rect"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(Present, "refresh"));
    try std.testing.expectEqual(@as(usize, 17), @offsetOf(Present, "present"));
}
