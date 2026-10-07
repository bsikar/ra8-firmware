//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Small generated 2-bit icon atlas and paint helper for UI widgets.

const std = @import("std");
const types = @import("widget_abi_types.zig");

/// Icons available to nav cells; `.none` leaves the cell text-only.
pub const Icon = enum(u8) {
    none = 0,
    back = 1,
    chevron_right = 2,
    play = 3,
    pause = 4,
    home = 5,
    library = 6,
    music = 7,
    settings = 8,
    search = 9,
};

const width: usize = 16;
const pixels_per_icon = width * width;
const packed_per_icon = pixels_per_icon / 4;
const atlas = @embedFile("icons_atlas.bin");
const header_bytes: usize = 8;

/// Return one 2-bit atlas sample; values range from transparent to full ink.
pub fn sample(icon: Icon, x: usize, y: usize) u2 {
    if (icon == .none or x >= width or y >= width) return 0;
    const index: usize = @backingInt(icon) - 1;
    const pixel = y * width + x;
    const packed_byte = atlas[header_bytes + index * packed_per_icon + pixel / 4];
    const shift: u3 = @intCast(6 - (pixel % 4) * 2);
    return @intCast((packed_byte >> shift) & 0x03);
}

fn blend(fg: u32, bg: u32, coverage: u2) u32 {
    const alpha: u32 = coverage;
    var color: u32 = 0;
    inline for (.{ @as(u5, 16), @as(u5, 8), @as(u5, 0) }) |shift| {
        const front = (fg >> shift) & 0xff;
        const back = (bg >> shift) & 0xff;
        const channel = (front * alpha + back * (3 - alpha) + 1) / 3;
        color |= channel << shift;
    }
    return color;
}

/// Paint an icon centered and nearest-neighbor scaled into .
pub fn draw(backend: *const types.Paint, icon: Icon, rect: types.Rect, fg: u32, bg: u32) void {
    const fill = backend.fill_rect orelse return;
    if (icon == .none or rect.w <= 0 or rect.h <= 0) return;
    const side = @min(width, @as(usize, @intCast(@min(rect.w, rect.h))));
    if (side == 0) return;
    const out_side: i32 = @intCast(side);
    const left = rect.x + @divTrunc(rect.w - out_side, 2);
    const top = rect.y + @divTrunc(rect.h - out_side, 2);

    var y: usize = 0;
    while (y < side) : (y += 1) {
        var x: usize = 0;
        while (x < side) {
            const source_y = y * width / side;
            const source_x = x * width / side;
            const coverage = sample(icon, source_x, source_y);
            if (coverage == 0) {
                x += 1;
                continue;
            }
            const color = blend(fg, bg, coverage);
            var end = x + 1;
            while (end < side and sample(icon, end * width / side, source_y) == coverage) : (end += 1) {}
            fill(backend.user, left + @as(i32, @intCast(x)), top + @as(i32, @intCast(y)), @intCast(end - x), 1, color);
            x = end;
        }
    }
}

comptime {
    if (atlas.len != header_bytes + 9 * packed_per_icon) @compileError("widget icon atlas byte length");
    if (!std.mem.eql(u8, atlas[0..4], "R8IA")) @compileError("widget icon atlas magic");
    if (atlas[4] != 1 or atlas[5] != 9 or atlas[6] != 16 or atlas[7] != 0) @compileError("widget icon atlas header");
}
