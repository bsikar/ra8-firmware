// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Tap-driven EPUB figure-zoom popup controller over `zoom_view_t`.
//!
//! Pairs a strip-composited zoomed viewport with a top status/close bar and
//! `mg_reader`-style 5-zone edge pan / centre zoom touch handling so an
//! e-reader page viewer can open an inline raster figure in a zero-heap popup
//! and dismiss back to the reading page on tap.

const std = @import("std");
pub const zoom = @import("zoom.zig");
pub const render = @import("zoom_render.zig");

const Err = zoom.Err;
const Source = zoom.Source;
const Scratch = zoom.Scratch;
const Rect = zoom.Rect;
const View = zoom.View;
const Cfg = zoom.Cfg;
const Present = zoom.Present;
const Policy = zoom.Policy;

pub const bar_h_default: u16 = 48;
pub const close_w_default: u16 = 112;
pub const edge_band_default: u16 = 176;

const col_bar_bg: u32 = 0xFFFFFF;
const col_bar_fg: u32 = 0x000000;
const col_btn_bg: u32 = 0x000000;
const col_btn_fg: u32 = 0xFFFFFF;
const face_sans: u8 = 0;
const glyph_w: i32 = 8;
const glyph_h: i32 = 16;
const bar_pad_x: i32 = 8;
const btn_inset: i32 = 4;

extern fn ra8_gfx_rect(x: i32, y: i32, w: i32, h: i32, color: u32, filled: bool) callconv(.c) Err;
extern fn ra8_gfx_text_out_face(x: i32, y: i32, str: [*:0]const u8, face: u8, fg: u32, bg: u32) callconv(.c) Err;

/// Discrete tap zones (`zoom_popup_zone_t`).
pub const PopupZone = enum(u8) {
    none = 0,
    pan_up = 1,
    pan_down = 2,
    pan_left = 3,
    pan_right = 4,
    zoom = 5,
    close = 6,
};

/// Result of handling one tap (`zoom_popup_action_t`).
pub const PopupAction = enum(u8) {
    none = 0,
    redrawn = 1,
    closed = 2,
};

/// Opening configuration (`zoom_popup_cfg_t`).
pub const PopupCfg = extern struct {
    src: Source = .{},
    scratch: Scratch = .{},
    fb_w: u16 = 0,
    fb_h: u16 = 0,
    bar_h: u16 = 0,
    close_w: u16 = 0,
    edge_band: u16 = 0,
    scale: u8 = 0,
    scale_max: u8 = 0,
    policy: Policy = .responsive,
    settle_ms: u16 = 0,
    focus_x: i32 = 0,
    focus_y: i32 = 0,
    tone: ?*const anyopaque = null,
};

/// Live popup controller state (`zoom_popup_t`).
pub const Popup = extern struct {
    view: View = .{},
    fb_w: u16 = 0,
    fb_h: u16 = 0,
    bar_h: u16 = 0,
    close_w: u16 = 0,
    edge_band: u16 = 0,
    chrome_dirty: bool = false,
    active: bool = false,
};

fn resolveBarH(cfg: *const PopupCfg) u16 {
    if (cfg.bar_h != 0) return cfg.bar_h;
    return @min(bar_h_default, cfg.fb_h / 3);
}

fn resolveCloseW(cfg: *const PopupCfg) u16 {
    if (cfg.close_w != 0) return @min(cfg.close_w, cfg.fb_w);
    return @min(close_w_default, cfg.fb_w / 2);
}

fn resolveEdgeBand(cfg: *const PopupCfg, view_h: u16) u16 {
    if (cfg.edge_band != 0) return cfg.edge_band;
    const min_dim = @min(cfg.fb_w, view_h);
    return @min(edge_band_default, min_dim / 3);
}

/// Open a figure-zoom popup over a bound source (`zoom_popup_open`).
pub fn popupOpen(p: ?*Popup, cfg: ?*const PopupCfg) Err {
    if (p == null or cfg == null) {
        zoom.logErr("popup open null argument");
        return .null_ptr;
    }
    p.?.active = false;
    p.?.chrome_dirty = false;
    const c = cfg.?;
    if (c.fb_w == 0 or c.fb_h < 2) {
        zoom.logErr("popup framebuffer too small");
        return .invalid_arg;
    }
    const bar_h = resolveBarH(c);
    if (bar_h >= c.fb_h) {
        zoom.logErr("popup bar covers framebuffer");
        return .invalid_arg;
    }
    const view_h: u16 = c.fb_h - bar_h;
    const vcfg = Cfg{
        .src = c.src,
        .scratch = c.scratch,
        .dst = .{ .x = 0, .y = bar_h, .w = c.fb_w, .h = view_h },
        .scale = c.scale,
        .scale_max = c.scale_max,
        .policy = c.policy,
        .settle_ms = c.settle_ms,
        .focus_x = c.focus_x,
        .focus_y = c.focus_y,
        .tone = c.tone,
    };
    const err = zoom.viewOpen(&p.?.view, &vcfg);
    if (err != .ok) return err;
    p.?.fb_w = c.fb_w;
    p.?.fb_h = c.fb_h;
    p.?.bar_h = bar_h;
    p.?.close_w = resolveCloseW(c);
    p.?.edge_band = resolveEdgeBand(c, view_h);
    p.?.chrome_dirty = true;
    p.?.active = true;
    return .ok;
}

/// Dismiss an active popup (`zoom_popup_close`).
pub fn popupClose(p: ?*Popup) Err {
    if (p == null) {
        zoom.logErr("popup must not be nullptr");
        return .null_ptr;
    }
    zoom.close(&p.?.view);
    p.?.chrome_dirty = false;
    p.?.active = false;
    return .ok;
}

/// Hit-test `(x, y)` against the top close chip and 5-zone viewport (`zoom_popup_zone_hit`).
pub fn popupZoneHit(p: ?*const Popup, x: i32, y: i32) PopupZone {
    if (p == null or !p.?.active) return .none;
    const s = p.?;
    if (x < 0 or y < 0 or x >= s.fb_w or y >= s.fb_h) return .none;
    if (y < s.bar_h) {
        return if (s.close_w > 0 and x < s.close_w) .close else .none;
    }
    const band: i32 = s.edge_band;
    const vy_min: i32 = @as(i32, s.bar_h) + band;
    if (y < vy_min) return .pan_up;
    if (y >= @as(i32, s.fb_h) - band) return .pan_down;
    if (x < band) return .pan_left;
    if (x >= @as(i32, s.fb_w) - band) return .pan_right;
    return .zoom;
}

fn zoneToPan(zone: PopupZone) zoom.Pan {
    return switch (zone) {
        .pan_up => .up,
        .pan_down => .down,
        .pan_left => .left,
        .pan_right => .right,
        else => .none,
    };
}

/// Handle one panel tap (`zoom_popup_tap`).
pub fn popupTap(p: ?*Popup, x: i32, y: i32, now_ms: u32, out_action: ?*PopupAction) Err {
    if (p == null or out_action == null) {
        zoom.logErr("popup tap null argument");
        return .null_ptr;
    }
    out_action.?.* = .none;
    if (!p.?.active) {
        zoom.logErr("tap on closed popup");
        return .invalid_state;
    }
    const s = p.?;
    const zone = popupZoneHit(s, x, y);
    switch (zone) {
        .none => return .ok,
        .close => {
            _ = popupClose(s);
            out_action.?.* = .closed;
            return .ok;
        },
        .pan_up, .pan_down, .pan_left, .pan_right => {
            const ox = s.view.anchor_x;
            const oy = s.view.anchor_y;
            const err = zoom.viewPanDir(&s.view, zoneToPan(zone), now_ms);
            if (err != .ok) return err;
            if ((s.view.anchor_x != ox) or (s.view.anchor_y != oy)) {
                out_action.?.* = .redrawn;
            }
            return .ok;
        },
        .zoom => {
            const next = zoom.scaleCycle(s.view.scale, @intCast(zoom.scale_min), s.view.scale_max);
            if (next == s.view.scale) return .ok;
            const err = zoom.viewSetScale(&s.view, next, x, y, now_ms);
            if (err != .ok) return err;
            s.chrome_dirty = true;
            out_action.?.* = .redrawn;
            return .ok;
        },
    }
}

/// Advance the responsive settle timer (`zoom_popup_tick`).
pub fn popupTick(p: ?*Popup, now_ms: u32) bool {
    if (p == null or !p.?.active) return false;
    return zoom.viewTick(&p.?.view, now_ms);
}

/// Consume the pending refresh rectangle and waveform class (`zoom_popup_present`).
pub fn popupPresent(p: ?*Popup, out: ?*Present) Err {
    if (p == null or out == null) {
        zoom.logErr("popup present null argument");
        return .null_ptr;
    }
    if (!p.?.active) {
        zoom.logErr("present on closed popup");
        return .invalid_state;
    }
    const err = zoom.viewPresent(&p.?.view, out);
    if (err != .ok) return err;
    if (p.?.chrome_dirty) {
        out.?.rect = .{ .x = 0, .y = 0, .w = p.?.fb_w, .h = p.?.fb_h };
        if (!out.?.present) {
            out.?.present = true;
            out.?.refresh = .quality;
        }
        p.?.chrome_dirty = false;
    }
    return .ok;
}

fn appendLiteral(buf: []u8, pos: *usize, text: []const u8) bool {
    if (pos.* + text.len > buf.len) return false;
    @memcpy(buf[pos.* .. pos.* + text.len], text);
    pos.* += text.len;
    return true;
}

fn appendDecU32(buf: []u8, pos: *usize, val: u32) bool {
    var tmp: [10]u8 = undefined;
    var n: usize = 0;
    var v = val;
    while (true) {
        tmp[n] = @intCast('0' + (v % 10));
        n += 1;
        v /= 10;
        if (v == 0) break;
    }
    if (pos.* + n > buf.len) return false;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        buf[pos.* + i] = tmp[n - 1 - i];
    }
    pos.* += n;
    return true;
}

/// Format `"FIG WxH Sx"` NUL-terminated status text into caller storage (`zoom_popup_status`).
pub fn popupStatus(p: ?*const Popup, buf: ?[*]u8, buf_cap: usize) Err {
    if (p == null or buf == null) {
        zoom.logErr("popup status null argument");
        return .null_ptr;
    }
    if (!p.?.active) {
        zoom.logErr("status on closed popup");
        return .invalid_state;
    }
    if (buf_cap == 0) return .no_mem;
    const slice = buf.?[0 .. buf_cap - 1];
    var pos: usize = 0;
    const s = p.?;
    const ok = appendLiteral(slice, &pos, "FIG ") and
        appendDecU32(slice, &pos, s.view.src.width) and
        appendLiteral(slice, &pos, "x") and
        appendDecU32(slice, &pos, s.view.src.height) and
        appendLiteral(slice, &pos, " ") and
        appendDecU32(slice, &pos, s.view.scale) and
        appendLiteral(slice, &pos, "x");
    if (!ok) return .no_mem;
    buf.?[pos] = 0;
    return .ok;
}

fn drawBoundedText(x: i32, y: i32, text: []const u8, max_w: i32, fg: u32, bg: u32) Err {
    if (max_w < glyph_w or text.len == 0) return .ok;
    const max_chars: usize = @intCast(@divTrunc(max_w, glyph_w));
    const n = @min(text.len, @min(max_chars, 47));
    if (n == 0) return .ok;
    var tmp: [48]u8 = undefined;
    @memcpy(tmp[0..n], text[0..n]);
    tmp[n] = 0;
    return ra8_gfx_text_out_face(x, y, @ptrCast(&tmp), face_sans, fg, bg);
}

fn drawChrome(s: *const Popup) Err {
    const bw: i32 = s.fb_w;
    const bh: i32 = s.bar_h;
    var err = ra8_gfx_rect(0, 0, bw, bh, col_bar_bg, true);
    if (err != .ok) return err;
    err = ra8_gfx_rect(0, bh - 1, bw, 1, col_bar_fg, true);
    if (err != .ok) return err;
    const cw: i32 = @min(@as(i32, s.close_w), bw);
    if (cw > (2 * btn_inset)) {
        const bx = btn_inset;
        const by = btn_inset;
        const b_w = cw - (2 * btn_inset);
        const b_h = @max(1, bh - (2 * btn_inset));
        err = ra8_gfx_rect(bx, by, b_w, b_h, col_btn_bg, true);
        if (err != .ok) return err;
        if (bh >= glyph_h + (2 * btn_inset)) {
            const ty = @divTrunc(bh - glyph_h, 2);
            err = drawBoundedText(bx + bar_pad_x, ty, "CLOSE", b_w - (2 * bar_pad_x), col_btn_fg, col_btn_bg);
            if (err != .ok) return err;
        }
    }
    if (bh >= glyph_h) {
        var status: [48]u8 = undefined;
        if (popupStatus(s, &status, status.len) == .ok) {
            const len = std.mem.indexOfScalar(u8, &status, 0) orelse 0;
            const tx: i32 = cw + bar_pad_x;
            const ty: i32 = @divTrunc(bh - glyph_h, 2);
            err = drawBoundedText(tx, ty, status[0..len], bw - tx - bar_pad_x, col_bar_fg, col_bar_bg);
            if (err != .ok) return err;
        }
    }
    return .ok;
}

/// Composite chrome bar and zoomed image into the bound `ra8_gfx` framebuffer (`zoom_popup_render`).
pub fn popupRender(p: ?*Popup) Err {
    if (p == null) {
        zoom.logErr("popup must not be nullptr");
        return .null_ptr;
    }
    if (!p.?.active) {
        zoom.logErr("render on closed popup");
        return .invalid_state;
    }
    const err = render.viewRender(&p.?.view);
    if (err != .ok) return err;
    return drawChrome(p.?);
}
