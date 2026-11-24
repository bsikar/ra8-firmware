// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the tap-driven EPUB figure-zoom popup (`src/zoom_popup.zig`).

const std = @import("std");
const popup = @import("popup");
const stubs = @import("zoom_stubs.zig");

const C = stubs.ChromeStubs(popup.zoom.Err);

comptime {
    _ = @import("zoom_stubs.zig");
    _ = stubs.LogSink();
    _ = stubs.DitherBlitStubs(popup.render);
    _ = C;
}

const testing = std.testing;
const Err = popup.zoom.Err;
const Present = popup.zoom.Present;
const Popup = popup.Popup;
const PopupCfg = popup.PopupCfg;
const PopupZone = popup.PopupZone;
const PopupAction = popup.PopupAction;

test "popupOpen resolves mg_reader defaults on 1024x600 and small panels" {
    var row: [1024]u8 = undefined;
    var strip: [1024 * 4]u8 = undefined;
    var pbuf: [512 * 4]u8 = undefined;
    var p = Popup{};
    var cfg = PopupCfg{
        .src = .{ .read = C.stubRead, .width = 800, .height = 600 },
        .scratch = .{ .row = &row, .row_cap = 1024, .strip = &strip, .strip_cap = strip.len, .packed_buf = &pbuf, .packed_cap = pbuf.len },
        .fb_w = 1024,
        .fb_h = 600,
    };
    try testing.expectEqual(Err.ok, popup.popupOpen(&p, &cfg));
    try testing.expect(p.active);
    try testing.expectEqual(@as(u16, 48), p.bar_h);
    try testing.expectEqual(@as(u16, 112), p.close_w);
    try testing.expectEqual(@as(u16, 176), p.edge_band);
    try testing.expectEqual(@as(i32, 48), p.view.dst.y);
    try testing.expectEqual(@as(i32, 552), p.view.dst.h);

    var small = Popup{};
    cfg.fb_w = 90;
    cfg.fb_h = 90;
    try testing.expectEqual(Err.ok, popup.popupOpen(&small, &cfg));
    try testing.expectEqual(@as(u16, 30), small.bar_h);
    try testing.expectEqual(@as(u16, 20), small.edge_band);
    try testing.expectEqual(PopupZone.zoom, popup.popupZoneHit(&small, 45, 60));
}

test "popupTap cycles zoom, pans inside bounds, and closes on top-left chip" {
    var row: [96]u8 = undefined;
    var strip: [96 * 4]u8 = undefined;
    var pbuf: [48 * 4]u8 = undefined;
    var p = Popup{};
    const cfg = PopupCfg{
        .src = .{ .read = C.stubRead, .width = 160, .height = 120 },
        .scratch = .{ .row = &row, .row_cap = 96, .strip = &strip, .strip_cap = strip.len, .packed_buf = &pbuf, .packed_cap = pbuf.len },
        .fb_w = 96,
        .fb_h = 96,
        .scale_max = 4,
    };
    try testing.expectEqual(Err.ok, popup.popupOpen(&p, &cfg));
    var pres = Present{};
    try testing.expectEqual(Err.ok, popup.popupPresent(&p, &pres));
    try testing.expect(pres.present);
    try testing.expectEqual(popup.zoom.Refresh.quality, pres.refresh);
    try testing.expectEqual(@as(i32, 0), pres.rect.y);

    var act: PopupAction = .none;
    try testing.expectEqual(Err.ok, popup.popupTap(&p, 80, 10, 100, &act));
    try testing.expectEqual(PopupAction.none, act);
    try testing.expectEqual(Err.ok, popup.popupTap(&p, 48, 64, 150, &act));
    try testing.expectEqual(PopupAction.redrawn, act);
    try testing.expectEqual(@as(u8, 2), p.view.scale);
    try testing.expect(p.chrome_dirty);
    try testing.expectEqual(Err.ok, popup.popupPresent(&p, &pres));
    try testing.expectEqual(@as(i32, 0), pres.rect.y);
    try testing.expectEqual(Err.ok, popup.popupTap(&p, 90, 64, 200, &act));
    try testing.expectEqual(PopupAction.redrawn, act);
    try testing.expect(!p.chrome_dirty);
    try testing.expectEqual(Err.ok, popup.popupPresent(&p, &pres));
    try testing.expectEqual(@as(i32, p.bar_h), pres.rect.y);
    try testing.expectEqual(popup.zoom.Refresh.fast, pres.refresh);
    try testing.expect(!popup.popupTick(&p, 250));
    try testing.expect(popup.popupTick(&p, 600));
    try testing.expectEqual(Err.ok, popup.popupPresent(&p, &pres));
    try testing.expectEqual(popup.zoom.Refresh.quality, pres.refresh);
    try testing.expectEqual(Err.ok, popup.popupTap(&p, 10, 10, 700, &act));
    try testing.expectEqual(PopupAction.closed, act);
    try testing.expect(!p.active);
    try testing.expectEqual(Err.invalid_state, popup.popupTap(&p, 48, 64, 710, &act));
}

test "popupRender draws chrome, status string, and zoomed image" {
    var row: [192]u8 = undefined;
    var strip: [192 * 4]u8 = undefined;
    var pbuf: [96 * 4]u8 = undefined;
    var p = Popup{};
    var cfg = PopupCfg{
        .src = .{ .read = C.stubRead, .width = 320, .height = 240 },
        .scratch = .{ .row = &row, .row_cap = 192, .strip = &strip, .strip_cap = strip.len, .packed_buf = &pbuf, .packed_cap = pbuf.len },
        .fb_w = 128,
        .fb_h = 96,
        .bar_h = 24,
        .close_w = 56,
    };
    try testing.expectEqual(Err.ok, popup.popupOpen(&p, &cfg));
    C.rect_calls = 0;
    C.text_calls = 0;
    try testing.expectEqual(Err.ok, popup.popupRender(&p));
    try testing.expect(C.rect_calls >= 3);
    try testing.expect(C.text_calls >= 2);
    try testing.expectEqualStrings("FIG 320", std.mem.span(@as([*:0]const u8, @ptrCast(&C.last_text))));

    cfg.fb_w = 192;
    try testing.expectEqual(Err.ok, popup.popupOpen(&p, &cfg));
    try testing.expectEqual(Err.ok, popup.popupRender(&p));
    try testing.expectEqualStrings("FIG 320x240 1x", std.mem.span(@as([*:0]const u8, @ptrCast(&C.last_text))));
}
