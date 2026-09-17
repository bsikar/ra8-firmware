//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the C ABI membrane: the argument guards, the guard
//! ORDER the C suite's MC/DC vectors depend on, the `ra8_err_t` mapping and
//! the injected LCD / touch seams driven from stubs.

const std = @import("std");
const abi = @import("abi");

const Point = abi.Point;
const Matrix = abi.Matrix;

fn code(value: abi.CalError) u16 {
    return @intFromEnum(value);
}

const identity: Matrix = .{ .a = 1, .b = 0, .c = 0, .d = 0, .e = 1, .f = 0 };

/// Stub state behind both seams: records what was painted and replays a
/// scripted list of raw samples, the same shape as the C suite's stub.
const Stub = struct {
    draws: [abi.n_targets]Point = @splat(.{}),
    n_draws: u8 = 0,
    reads: []const Point = &.{},
    reads_idx: u8 = 0,
    forced_err: u16 = 0,

    fn draw(ctx: ?*anyopaque, target: Point) callconv(.c) u16 {
        const self: *Stub = @ptrCast(@alignCast(ctx.?));
        if (self.forced_err != 0) return self.forced_err;
        if (self.n_draws < abi.n_targets) {
            self.draws[self.n_draws] = target;
            self.n_draws += 1;
        }
        return 0;
    }

    fn read(ctx: ?*anyopaque, out_raw: *Point) callconv(.c) u16 {
        const self: *Stub = @ptrCast(@alignCast(ctx.?));
        if (self.reads_idx >= self.reads.len) return code(.hw_error);
        out_raw.* = self.reads[self.reads_idx];
        self.reads_idx += 1;
        return 0;
    }

    fn config(self: *Stub, w: u16, h: u16, inset: u16) abi.RunConfig {
        return .{
            .screen_width = w,
            .screen_height = h,
            .inset_px = inset,
            .draw_target = &draw,
            .draw_ctx = self,
            .read_raw = &read,
            .read_ctx = self,
        };
    }
};

test "compute rejects each null pointer in declaration order" {
    var pts = [_]Point{.{}} ** 5;
    var m: Matrix = .{};
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_compute(null, &pts, 5, &m));
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_compute(&pts, null, 5, &m));
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_compute(&pts, &pts, 5, null));
}

test "compute rejects a sample count outside the accepted range" {
    var pts = [_]Point{.{}} ** 6;
    var m: Matrix = .{};
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_compute(&pts, &pts, 2, &m));
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_compute(&pts, &pts, 6, &m));
}

test "compute reports a singular system as invalid_arg" {
    var raw = [_]Point{ .{ .x = 0, .y = 0 }, .{ .x = 100, .y = 100 }, .{ .x = 200, .y = 200 } };
    var screen = [_]Point{ .{ .x = 0, .y = 0 }, .{ .x = 10, .y = 10 }, .{ .x = 20, .y = 20 } };
    var m: Matrix = .{};
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_compute(&raw, &screen, 3, &m));
}

test "compute leaves the destination untouched when it refuses" {
    var pts = [_]Point{.{}} ** 5;
    var m: Matrix = .{ .a = 7, .b = 7, .c = 7, .d = 7, .e = 7, .f = 7 };
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_compute(&pts, &pts, 2, &m));
    try std.testing.expectEqual(@as(f32, 7), m.a);
}

test "compute publishes a matrix on the happy path" {
    var raw = [_]Point{
        .{ .x = 100, .y = 100 },
        .{ .x = 3900, .y = 100 },
        .{ .x = 2000, .y = 3900 },
    };
    var screen = [_]Point{ .{ .x = 0, .y = 0 }, .{ .x = 1023, .y = 0 }, .{ .x = 512, .y = 599 } };
    var m: Matrix = .{};
    try std.testing.expectEqual(code(.ok), abi.ra8_touch_cal_compute(&raw, &screen, 3, &m));
    try std.testing.expect(m.a != 0);
}

test "run rejects a null config or destination before touching a seam" {
    var stub: Stub = .{};
    var m: Matrix = .{};
    const cfg = stub.config(1024, 600, 32);
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_run(null, &m));
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_run(&cfg, null));
    try std.testing.expectEqual(@as(u8, 0), stub.n_draws);
}

test "run rejects an unbound seam as null_ptr, not invalid_arg" {
    var stub: Stub = .{};
    var m: Matrix = .{};
    var no_draw = stub.config(1024, 600, 32);
    no_draw.draw_target = null;
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_run(&no_draw, &m));

    var no_read = stub.config(1024, 600, 32);
    no_read.read_raw = null;
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_run(&no_read, &m));
}

test "run checks the seams before the panel geometry" {
    // Both guards would fire; the seam guard is first, so the caller sees
    // null_ptr. The C suite's MC/DC vectors pin this ordering.
    var stub: Stub = .{};
    var m: Matrix = .{};
    var cfg = stub.config(0, 600, 32);
    cfg.draw_target = null;
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_run(&cfg, &m));
}

test "run rejects a zero-sized panel on either axis" {
    var stub: Stub = .{};
    var m: Matrix = .{};
    const zero_w = stub.config(0, 600, 8);
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_run(&zero_w, &m));
    const zero_h = stub.config(320, 0, 8);
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_run(&zero_h, &m));
    try std.testing.expectEqual(@as(u8, 0), stub.n_draws);
}

test "run rejects an inset that collapses either axis" {
    var stub: Stub = .{};
    var m: Matrix = .{};
    const both = stub.config(100, 100, 60);
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_run(&both, &m));
    const height_only = stub.config(200, 100, 60);
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_run(&height_only, &m));
}

test "run maps any seam failure onto hw_error" {
    var samples = [_]Point{.{}} ** 5;
    var draw_fails: Stub = .{ .reads = &samples, .forced_err = code(.hw_error) };
    var m: Matrix = .{};
    const cfg_draw = draw_fails.config(1024, 600, 32);
    try std.testing.expectEqual(code(.hw_error), abi.ra8_touch_cal_run(&cfg_draw, &m));

    // A seam may hand back any code in the repo's error space; the caller's
    // recovery is identical, so the module collapses them all to hw_error.
    var odd_code: Stub = .{ .reads = &samples, .forced_err = code(.crc_mismatch) };
    const cfg_odd = odd_code.config(1024, 600, 32);
    try std.testing.expectEqual(code(.hw_error), abi.ra8_touch_cal_run(&cfg_odd, &m));

    var read_fails: Stub = .{ .reads = &.{} };
    const cfg_read = read_fails.config(1024, 600, 32);
    try std.testing.expectEqual(code(.hw_error), abi.ra8_touch_cal_run(&cfg_read, &m));
    try std.testing.expectEqual(@as(u8, 1), read_fails.n_draws);
}

test "run paints five targets in order and fits the samples" {
    const truth: Matrix = .{ .a = 0.25, .b = 0, .c = 0, .d = 0, .e = 0.20, .f = 0 };
    const targets = [_]Point{
        .{ .x = 32, .y = 32 },
        .{ .x = 991, .y = 32 },
        .{ .x = 991, .y = 567 },
        .{ .x = 32, .y = 567 },
        .{ .x = 512, .y = 300 },
    };
    var raw: [5]Point = @splat(.{});
    for (targets, 0..) |target, i| {
        raw[i] = .{
            .x = @intFromFloat(@as(f32, @floatFromInt(target.x)) / truth.a),
            .y = @intFromFloat(@as(f32, @floatFromInt(target.y)) / truth.e),
        };
    }

    var stub: Stub = .{ .reads = &raw };
    var m: Matrix = .{};
    const cfg = stub.config(1024, 600, 32);
    try std.testing.expectEqual(code(.ok), abi.ra8_touch_cal_run(&cfg, &m));
    try std.testing.expectEqual(@as(u8, 5), stub.n_draws);
    try std.testing.expectEqual(@as(u8, 5), stub.reads_idx);
    try std.testing.expectEqualSlices(Point, &targets, &stub.draws);

    for (raw, 0..) |point, i| {
        var mapped: Point = .{};
        try std.testing.expectEqual(code(.ok), abi.ra8_touch_cal_apply(point, &m, 1024, 600, &mapped));
        try std.testing.expect(@abs(mapped.x - targets[i].x) <= 5);
        try std.testing.expect(@abs(mapped.y - targets[i].y) <= 5);
    }
}

test "apply rejects each null pointer and a zero-sized panel" {
    var out: Point = .{};
    const raw: Point = .{};
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_apply(raw, null, 100, 100, &out));
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_apply(raw, &identity, 100, 100, null));
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_apply(raw, &identity, 0, 100, &out));
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_apply(raw, &identity, 100, 0, &out));
}

test "apply clips an identity transform to the panel rectangle" {
    var out: Point = .{};
    try std.testing.expectEqual(
        code(.ok),
        abi.ra8_touch_cal_apply(.{ .x = -50, .y = -50 }, &identity, 100, 100, &out),
    );
    try std.testing.expectEqual(Point{ .x = 0, .y = 0 }, out);
    try std.testing.expectEqual(
        code(.ok),
        abi.ra8_touch_cal_apply(.{ .x = 9999, .y = 9999 }, &identity, 100, 100, &out),
    );
    try std.testing.expectEqual(Point{ .x = 99, .y = 99 }, out);
}

test "save rejects each null pointer and a short buffer" {
    var blob: [abi.blob_size]u8 = @splat(0);
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_save(null, &blob, blob.len));
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_save(&identity, null, blob.len));
    try std.testing.expectEqual(
        code(.invalid_size),
        abi.ra8_touch_cal_save(&identity, &blob, abi.blob_size - 1),
    );
}

test "load rejects each null pointer and a short buffer" {
    var blob: [abi.blob_size]u8 = @splat(0);
    var out: Matrix = .{};
    try std.testing.expectEqual(code(.ok), abi.ra8_touch_cal_save(&identity, &blob, blob.len));
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_load(null, blob.len, &out));
    try std.testing.expectEqual(code(.null_ptr), abi.ra8_touch_cal_load(&blob, blob.len, null));
    try std.testing.expectEqual(
        code(.invalid_size),
        abi.ra8_touch_cal_load(&blob, abi.blob_size - 1, &out),
    );
}

test "save then load round-trips every coefficient" {
    const m: Matrix = .{ .a = 0.25, .b = 0.001, .c = 5.5, .d = -0.01, .e = 0.19, .f = -3.25 };
    var blob: [abi.blob_size]u8 = @splat(0);
    try std.testing.expectEqual(code(.ok), abi.ra8_touch_cal_save(&m, &blob, blob.len));
    try std.testing.expectEqualSlices(u8, "TCAL", blob[0..4]);

    var out: Matrix = .{};
    try std.testing.expectEqual(code(.ok), abi.ra8_touch_cal_load(&blob, blob.len, &out));
    try std.testing.expectEqual(m, out);
}

test "load maps a bad header to invalid_arg and a bad body to crc_mismatch" {
    var blob: [abi.blob_size]u8 = @splat(0);
    var out: Matrix = .{};
    try std.testing.expectEqual(code(.ok), abi.ra8_touch_cal_save(&identity, &blob, blob.len));

    var bad_magic = blob;
    bad_magic[0] = 'X';
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_load(&bad_magic, blob.len, &out));

    var bad_version = blob;
    bad_version[4] = 0xFF;
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_load(&bad_version, blob.len, &out));

    var bad_reserved = blob;
    bad_reserved[5] = 0x42;
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_load(&bad_reserved, blob.len, &out));

    var bad_coeff = blob;
    bad_coeff[8] ^= 0xFF;
    try std.testing.expectEqual(code(.crc_mismatch), abi.ra8_touch_cal_load(&bad_coeff, blob.len, &out));
}

test "a refused load leaves the caller's matrix alone" {
    var blob: [abi.blob_size]u8 = @splat(0);
    var out: Matrix = .{ .a = 9, .b = 9, .c = 9, .d = 9, .e = 9, .f = 9 };
    try std.testing.expectEqual(code(.invalid_arg), abi.ra8_touch_cal_load(&blob, blob.len, &out));
    try std.testing.expectEqual(@as(f32, 9), out.a);
}

test "save accepts a destination larger than the blob and writes only the blob" {
    var big: [abi.blob_size * 2]u8 = @splat(0xAA);
    try std.testing.expectEqual(code(.ok), abi.ra8_touch_cal_save(&identity, &big, big.len));
    for (big[abi.blob_size..]) |byte| try std.testing.expectEqual(@as(u8, 0xAA), byte);
}
