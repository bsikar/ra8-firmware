//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host framebuffer and typography tests for `overlay.zig` over real
//! `ra8_gfx` surfaces (`Surface` ARGB8888 and `EinkSurface` packed gray4)
//! and `ra8_widget` Paint ABI parity.

const std = @import("std");
const overlay = @import("overlay");
const sidecar = @import("sidecar");
const gfx = @import("ra8_gfx");
const panel_paint = @import("panel_paint");
const widget_paint_abi = @import("widget_paint_abi");

comptime {
    if (@sizeOf(overlay.Rect) != @sizeOf(widget_paint_abi.Rect)) @compileError("Rect size mismatch");
    if (@sizeOf(overlay.CanvasCaps) != @sizeOf(widget_paint_abi.CanvasCaps)) @compileError("CanvasCaps size mismatch");
    if (@sizeOf(overlay.Paint) != @sizeOf(widget_paint_abi.Paint)) @compileError("Paint size mismatch");
    if (@alignOf(overlay.Paint) != @alignOf(widget_paint_abi.Paint)) @compileError("Paint align mismatch");
    for (.{
        "user",
        "fill_rect",
        "draw_text",
        "text_size",
        "draw_text_face",
        "text_size_face",
        "draw_text_style",
        "text_size_style",
        "draw_line",
        "blit",
        "push_clip",
        "pop_clip",
        "fill_rect_alpha",
        "caps",
    }) |field| {
        if (@offsetOf(overlay.Paint, field) != @offsetOf(widget_paint_abi.Paint, field)) {
            @compileError("Paint offset mismatch on " ++ field);
        }
    }
}

fn drawMangaPageArt(surf: anytype, w: i32, h: i32) void {
    surf.fill(.{ .x = 0, .y = 0, .w = w, .h = h }, 0xD8D8D8);
    // Panel borders and cross-hatching artwork so untouched regions stay verifiable.
    surf.fill(.{ .x = 8, .y = 8, .w = w - 16, .h = 4 }, 0x202020);
    surf.fill(.{ .x = 8, .y = h - 12, .w = w - 16, .h = 4 }, 0x202020);
    var row: i32 = 24;
    while (row < h - 24) : (row += 12) {
        surf.line(12, row, w - 12, row, 0x707070);
    }
    surf.fill(.{ .x = 180, .y = 40, .w = 40, .h = 300 }, 0x404040);
}

fn pointInAnyRect(x: i32, y: i32, rects: []const overlay.Rect) bool {
    for (rects) |r| {
        if (x >= r.x and y >= r.y and x < r.x + r.w and y < r.y + r.h) return true;
    }
    return false;
}

fn trimAsciiWs(slice: []const u8) []const u8 {
    var start: usize = 0;
    while (start < slice.len and (slice[start] == ' ' or slice[start] == '\t' or slice[start] == '\r' or slice[start] == '\n')) : (start += 1) {}
    var end = slice.len;
    while (end > start and (slice[end - 1] == ' ' or slice[end - 1] == '\t' or slice[end - 1] == '\r' or slice[end - 1] == '\n')) : (end -= 1) {}
    return slice[start..end];
}

fn countNonWhiteInRect(surf: *const gfx.Surface, r: overlay.Rect) usize {
    var dark: usize = 0;
    var y = r.y;
    while (y < r.y + r.h) : (y += 1) {
        var x = r.x;
        while (x < r.x + r.w) : (x += 1) {
            if (x < 0 or y < 0 or x >= @as(i32, @intCast(surf.width)) or y >= @as(i32, @intCast(surf.height))) continue;
            if ((surf.at(@intCast(x), @intCast(y)) & 0xFFFFFF) != 0xFFFFFF) dark += 1;
        }
    }
    return dark;
}

fn referenceDrawLines(
    target: anytype,
    paint: *const overlay.Paint,
    spec: overlay.BubbleSpec,
    style: overlay.Style,
    size_enum: u8,
    font_px: u16,
    eng: []const u8,
    breaks: []const u16,
) !void {
    const safe = try overlay.safeTextRect(spec, style);
    const damage = overlay.Rect{ .x = spec.x, .y = spec.y, .w = spec.w, .h = spec.h };
    target.pushClip(.{ .x = damage.x, .y = damage.y, .w = damage.w, .h = damage.h });
    defer target.popClip();

    overlay.fillShapeMask(paint, damage, sidecar.Shape.fromRaw(spec.shape).?, spec.corner_radius, style.bg_color);
    if (eng.len == 0) return;

    const block_h: i32 = @intCast(breaks.len * font_px);
    var y = safe.y + @divFloor(@max(safe.h - block_h, 0), 2);
    var prev: usize = 0;
    var zbuf: [513]u8 = undefined;
    for (breaks) |end_u16| {
        const end: usize = @intCast(end_u16);
        const line = trimAsciiWs(eng[prev..end]);
        if (line.len > 0) {
            @memcpy(zbuf[0..line.len], line);
            zbuf[line.len] = 0;
            const cstr: [*:0]const u8 = @ptrCast(&zbuf);
            const ext = if (size_enum != 0)
                gfx.Surface.measureTextStyle(cstr, .sans, .regular, size_enum)
            else
                gfx.Surface.measureText(cstr, .sans);
            const lw: i32 = @intCast(ext.width);
            const x = safe.x + @divFloor(safe.w - lw, 2);
            if (size_enum != 0) {
                target.drawTextStyle(cstr, x, y, .sans, .regular, size_enum, style.fg_color, style.bg_color);
            } else {
                target.drawText(cstr, x, y, style.fg_color, style.bg_color);
            }
        }
        prev = end;
        y += @intCast(font_px);
    }
}

test "fitText selects largest fitting font across wide, medium, and tall-narrow vertical bubbles" {
    var surface = try gfx.Surface.init(std.testing.allocator, 64, 64, 0xFFFFFF);
    defer surface.deinit(std.testing.allocator);
    const paint = panel_paint.table(gfx.Surface, overlay.Paint, &surface, gfx.Capabilities.desktop_argb8888);
    const style = overlay.Style{};

    var breaks: [overlay.max_lines]u16 = undefined;
    const big = try overlay.fitText(
        &paint,
        .{ .x = 0, .y = 0, .w = 340, .h = 200, .shape = @backingInt(sidecar.Shape.rect) },
        style,
        "RUN!",
        &breaks,
    );
    try std.testing.expectEqual(@as(u8, @backingInt(overlay.TextSize.title_68)), big.size_enum);
    try std.testing.expectEqual(@as(u16, 81), big.font_size_px);
    try std.testing.expectEqual(@as(u16, 1), big.line_count);
    try std.testing.expectEqual(@as(u8, 0), big.fit_flags);

    const medium = try overlay.fitText(
        &paint,
        .{ .x = 0, .y = 0, .w = 210, .h = 110, .shape = @backingInt(sidecar.Shape.rounded_rect), .corner_radius = 12 },
        style,
        "Wait until morning comes.",
        &breaks,
    );
    try std.testing.expect(medium.font_size_px < big.font_size_px);
    try std.testing.expect(medium.font_size_px >= 32);
    try std.testing.expect(medium.line_count >= 2);
    try std.testing.expectEqual(@as(u8, 0), medium.fit_flags);

    // Tall narrow Japanese vertical speech bubble (w=72, h=220) forces small wrapped lines cleanly.
    const narrow_text = "Hold on, wait for me!";
    const narrow = try overlay.fitText(
        &paint,
        .{
            .x = 10,
            .y = 20,
            .w = 72,
            .h = 220,
            .shape = @backingInt(sidecar.Shape.ellipse),
            .text_orientation = @backingInt(sidecar.TextOrientation.vertical_rl),
        },
        style,
        narrow_text,
        &breaks,
    );
    try std.testing.expectEqual(@as(u16, 16), narrow.font_size_px);
    try std.testing.expectEqual(@as(u8, 0), narrow.size_enum);
    try std.testing.expect(narrow.line_count >= 4);
    try std.testing.expectEqual(@as(u8, 0), narrow.fit_flags);
    try std.testing.expectEqual(@as(u16, narrow_text.len), breaks[narrow.line_count - 1]);

    // Ultra-narrow strip forcing single long word hard-break records hard_wrapped flag cleanly.
    const hard_text = "Extraordinary";
    const hard = try overlay.fitText(
        &paint,
        .{ .x = 0, .y = 0, .w = 48, .h = 160, .shape = @backingInt(sidecar.Shape.rect) },
        style,
        hard_text,
        &breaks,
    );
    try std.testing.expect((hard.fit_flags & overlay.fit_flag_hard_wrapped) != 0);
    try std.testing.expect(hard.line_count >= 3);
    try std.testing.expectEqual(@as(u16, hard_text.len), breaks[hard.line_count - 1]);
}

test "golden TRN1 page matches independent Surface and EinkSurface reference framebuffers byte for byte" {
    const page_w: u16 = 440;
    const page_h: u16 = 520;
    const style = overlay.Style{};

    var actual_rgb = try gfx.Surface.init(std.testing.allocator, page_w, page_h, 0xFFFFFF);
    defer actual_rgb.deinit(std.testing.allocator);
    var ref_rgb = try gfx.Surface.init(std.testing.allocator, page_w, page_h, 0xFFFFFF);
    defer ref_rgb.deinit(std.testing.allocator);
    var bg_snapshot = try gfx.Surface.init(std.testing.allocator, page_w, page_h, 0xFFFFFF);
    defer bg_snapshot.deinit(std.testing.allocator);

    drawMangaPageArt(&actual_rgb, page_w, page_h);
    drawMangaPageArt(&ref_rgb, page_w, page_h);
    drawMangaPageArt(&bg_snapshot, page_w, page_h);

    const eink_stride = gfx.eink_surface.packedRowBytes(page_w);
    const eink_bytes = try std.testing.allocator.alloc(u8, @as(usize, eink_stride) * page_h);
    defer std.testing.allocator.free(eink_bytes);
    const ref_eink_bytes = try std.testing.allocator.alloc(u8, @as(usize, eink_stride) * page_h);
    defer std.testing.allocator.free(ref_eink_bytes);
    var actual_eink = try gfx.EinkSurface.init(eink_bytes, page_w, page_h, eink_stride, 0xFFFFFF, null);
    var ref_eink = try gfx.EinkSurface.init(ref_eink_bytes, page_w, page_h, eink_stride, 0xFFFFFF, null);
    drawMangaPageArt(&actual_eink, page_w, page_h);
    drawMangaPageArt(&ref_eink, page_w, page_h);

    const paint_rgb = panel_paint.table(gfx.Surface, overlay.Paint, &actual_rgb, gfx.Capabilities.desktop_argb8888);
    const ref_paint_rgb = panel_paint.table(gfx.Surface, overlay.Paint, &ref_rgb, gfx.Capabilities.desktop_argb8888);
    const paint_eink = panel_paint.table(gfx.EinkSurface, overlay.Paint, &actual_eink, gfx.Capabilities.eink_gray4);
    const ref_paint_eink = panel_paint.table(gfx.EinkSurface, overlay.Paint, &ref_eink, gfx.Capabilities.eink_gray4);

    const specs = [_]overlay.BubbleSpec{
        .{ .x = 24, .y = 26, .w = 180, .h = 90, .shape = @backingInt(sidecar.Shape.rect), .text_orientation = 0 },
        .{ .x = 230, .y = 32, .w = 176, .h = 104, .shape = @backingInt(sidecar.Shape.rounded_rect), .text_orientation = 0, .corner_radius = 16 },
        .{ .x = 24, .y = 168, .w = 72, .h = 220, .shape = @backingInt(sidecar.Shape.ellipse), .text_orientation = @backingInt(sidecar.TextOrientation.vertical_rl) },
        .{ .x = 140, .y = 340, .w = 240, .h = 128, .shape = @backingInt(sidecar.Shape.cloud), .text_orientation = 0 },
    };
    const eng_texts = [_][]const u8{
        "Hey!",
        "Check the door now.",
        "Hold on, wait for me!",
        "Maybe tomorrow...",
    };

    var fits: [4]overlay.FitResult = undefined;
    var break_storage: [4][overlay.max_lines]u16 = undefined;
    for (specs, eng_texts, 0..) |sp, txt, idx| {
        fits[idx] = try overlay.fitText(&paint_rgb, sp, style, txt, &break_storage[idx]);
    }

    const inputs = [_]sidecar.BubbleInput{
        .{
            .x = specs[0].x,
            .y = specs[0].y,
            .w = specs[0].w,
            .h = specs[0].h,
            .shape = .rect,
            .text_orientation = .horizontal,
            .reading_order = 0,
            .ocr_confidence_bps = 9800,
            .font_size_px = fits[0].font_size_px,
            .line_breaks = break_storage[0][0..fits[0].line_count],
            .source_text = "ja0",
            .english_text = eng_texts[0],
        },
        .{
            .x = specs[1].x,
            .y = specs[1].y,
            .w = specs[1].w,
            .h = specs[1].h,
            .shape = .rounded_rect,
            .text_orientation = .horizontal,
            .corner_radius = 16,
            .reading_order = 2,
            .ocr_confidence_bps = 9500,
            .font_size_px = fits[1].font_size_px,
            .line_breaks = break_storage[1][0..fits[1].line_count],
            .source_text = "ja1",
            .english_text = eng_texts[1],
        },
        .{
            .x = specs[2].x,
            .y = specs[2].y,
            .w = specs[2].w,
            .h = specs[2].h,
            .shape = .ellipse,
            .text_orientation = .vertical_rl,
            .reading_order = 1,
            .ocr_confidence_bps = 9100,
            .font_size_px = fits[2].font_size_px,
            .line_breaks = break_storage[2][0..fits[2].line_count],
            .source_text = "ja2",
            .english_text = eng_texts[2],
        },
        .{
            .x = specs[3].x,
            .y = specs[3].y,
            .w = specs[3].w,
            .h = specs[3].h,
            .shape = .cloud,
            .text_orientation = .horizontal,
            .reading_order = 3,
            .ocr_confidence_bps = 8900,
            .font_size_px = fits[3].font_size_px,
            .line_breaks = break_storage[3][0..fits[3].line_count],
            .source_text = "ja3",
            .english_text = eng_texts[3],
        },
    };
    const pages = [_]sidecar.PageEntry{.{
        .page_index = 0,
        .bubble_count = inputs.len,
        .first_bubble = 0,
        .page_width = page_w,
        .page_height = page_h,
    }};
    const key = sidecar.SidecarKey{
        .chapter_hash = @splat(0x7A),
        .detector_model_version = 1,
        .ocr_model_version = 1,
        .nmt_model_version = 1,
        .glossary_revision = 0,
        .source_lang = @backingInt(sidecar.Language.japanese),
    };
    var trn1: [2048]u8 = undefined;
    const trn1_len = try sidecar.writeSidecarFromInputs(&key, &pages, &inputs, &trn1);

    var rgb_dmg: [4]overlay.Rect = undefined;
    const rgb_cnt = try overlay.renderPage(&paint_rgb, trn1[0..trn1_len], 0, style, &rgb_dmg);
    try std.testing.expectEqual(@as(usize, 4), rgb_cnt);

    var eink_dmg: [4]overlay.Rect = undefined;
    const eink_cnt = try overlay.renderPage(&paint_eink, trn1[0..trn1_len], 0, style, &eink_dmg);
    try std.testing.expectEqual(@as(usize, 4), eink_cnt);

    // Reference model renders the exact same reading order independently.
    const order_map = [_]usize{ 0, 2, 1, 3 };
    for (order_map) |idx| {
        try referenceDrawLines(
            &ref_rgb,
            &ref_paint_rgb,
            specs[idx],
            style,
            fits[idx].size_enum,
            fits[idx].font_size_px,
            eng_texts[idx],
            break_storage[idx][0..fits[idx].line_count],
        );
        try referenceDrawLines(
            &ref_eink,
            &ref_paint_eink,
            specs[idx],
            style,
            fits[idx].size_enum,
            fits[idx].font_size_px,
            eng_texts[idx],
            break_storage[idx][0..fits[idx].line_count],
        );
    }

    try std.testing.expectEqualSlices(u32, ref_rgb.pixels, actual_rgb.pixels);
    try std.testing.expectEqualSlices(u8, ref_eink.pixels, actual_eink.pixels);

    // Background art pixels outside the 4 bubble damage rects are 100% preserved.
    var y: i32 = 0;
    while (y < page_h) : (y += 1) {
        var x: i32 = 0;
        while (x < page_w) : (x += 1) {
            if (!pointInAnyRect(x, y, rgb_dmg[0..rgb_cnt])) {
                try std.testing.expectEqual(
                    bg_snapshot.at(@intCast(x), @intCast(y)),
                    actual_rgb.at(@intCast(x), @intCast(y)),
                );
            }
        }
    }

    // Shaped corners outside the rounded_rect/ellipse mask preserve original background art.
    try std.testing.expectEqual(bg_snapshot.at(230, 32), actual_rgb.at(230, 32));
    try std.testing.expectEqual(bg_snapshot.at(24, 168), actual_rgb.at(24, 168));

    // Text glyphs are genuinely rendered inside each bubble's safeTextRect (non-white ink present).
    for (specs) |sp| {
        const safe = try overlay.safeTextRect(sp, style);
        try std.testing.expect(countNonWhiteInRect(&actual_rgb, safe) > 20);
    }

    // E-ink gray4 nibbles inside empty white bubble margin vs drawn center glyphs.
    try std.testing.expectEqual(@as(u4, 0xF), actual_eink.nibbleAt(26, 28));
}

test "contained bubble deduplicates damage and undersized damage array fails without painting" {
    var surf = try gfx.Surface.init(std.testing.allocator, 200, 200, 0x102030);
    defer surf.deinit(std.testing.allocator);
    const paint = panel_paint.table(gfx.Surface, overlay.Paint, &surf, gfx.Capabilities.desktop_argb8888);
    const style = overlay.Style{};

    const outer = overlay.BubbleSpec{ .x = 20, .y = 20, .w = 140, .h = 120, .shape = @backingInt(sidecar.Shape.rect) };
    const inner = overlay.BubbleSpec{ .x = 40, .y = 40, .w = 80, .h = 40, .shape = @backingInt(sidecar.Shape.rect) };
    var br0: [overlay.max_lines]u16 = undefined;
    var br1: [overlay.max_lines]u16 = undefined;
    const fit0 = try overlay.fitText(&paint, outer, style, "Outer", &br0);
    const fit1 = try overlay.fitText(&paint, inner, style, "Inner", &br1);

    const inputs = [_]sidecar.BubbleInput{
        .{
            .x = outer.x,
            .y = outer.y,
            .w = outer.w,
            .h = outer.h,
            .shape = .rect,
            .text_orientation = .horizontal,
            .reading_order = 0,
            .ocr_confidence_bps = 9000,
            .font_size_px = fit0.font_size_px,
            .line_breaks = br0[0..fit0.line_count],
            .source_text = "a",
            .english_text = "Outer",
        },
        .{
            .x = inner.x,
            .y = inner.y,
            .w = inner.w,
            .h = inner.h,
            .shape = .rect,
            .text_orientation = .horizontal,
            .reading_order = 1,
            .ocr_confidence_bps = 9000,
            .font_size_px = fit1.font_size_px,
            .line_breaks = br1[0..fit1.line_count],
            .source_text = "b",
            .english_text = "Inner",
        },
    };
    const pages = [_]sidecar.PageEntry{.{
        .page_index = 0,
        .bubble_count = 2,
        .first_bubble = 0,
        .page_width = 200,
        .page_height = 200,
    }};
    const key = sidecar.SidecarKey{
        .chapter_hash = @splat(0x11),
        .detector_model_version = 1,
        .ocr_model_version = 1,
        .nmt_model_version = 1,
        .glossary_revision = 0,
        .source_lang = 1,
    };
    var trn1: [512]u8 = undefined;
    const trn1_len = try sidecar.writeSidecarFromInputs(&key, &pages, &inputs, &trn1);

    var none: [0]overlay.Rect = .{};
    try std.testing.expectError(overlay.Error.InvalidSize, overlay.renderPage(&paint, trn1[0..trn1_len], 0, style, &none));
    try std.testing.expectEqual(@as(u32, 0xFF102030), surf.at(30, 30));

    var one: [1]overlay.Rect = undefined;
    const n = try overlay.renderPage(&paint, trn1[0..trn1_len], 0, style, &one);
    try std.testing.expectEqual(@as(usize, 1), n);
    try std.testing.expectEqual(overlay.Rect{ .x = 20, .y = 20, .w = 140, .h = 120 }, one[0]);
}
