//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_touch_cal/inc/ra8_touch_cal.h`. The affine fit
//! and the storage codec live in `internal/root.zig`; this file owns the
//! injected LCD and touch seams, the argument guards and the `ra8_err_t`
//! mapping the C implementation exposed.
//!
//! No LCD or touch header is reachable from here. The guided calibration
//! sequence reaches hardware only through the two function pointers in the
//! run config, which is what keeps the whole module host-testable without the
//! register fake.
//!
//! Guard order is part of the contract, not an implementation detail: the
//! MC/DC vector sets in `tests/graphics/src/test_ra8_touch_cal.c` distinguish
//! `null_ptr` from `invalid_arg` by which guard fires first, so the checks
//! below run in exactly the order the C wrote them.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// One integer coordinate pair (`ra8_touch_cal_point_t`).
pub const Point = implementation.Point;
/// 2-D affine transform (`ra8_touch_cal_matrix_t`).
pub const Matrix = implementation.Matrix;
/// Target count the guided run paints (`k_ra8_touch_cal_n_targets`).
pub const n_targets = implementation.n_targets;
/// Serialised blob size in bytes (`k_ra8_touch_cal_blob_size`).
pub const blob_size = implementation.blob_size;

/// Subset of `ra8_err_t` this library returns.
pub const CalError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    invalid_size = 0x105,
    hw_error = 0x204,
    crc_mismatch = 0x405,
    null_ptr = 0x504,
};

/// Raw `ra8_err_t` as it crosses the ABI, so a seam may hand back any code in
/// the repo's error space and have it propagate unchanged.
pub const RawErr = u16;

/// LCD seam: paint a cross-hair at a screen coordinate
/// (`ra8_touch_cal_draw_target_fn_t`).
pub const DrawTargetFn = *const fn (ctx: ?*anyopaque, target: Point) callconv(.c) RawErr;
/// Touch seam: block until one stable raw sample is captured
/// (`ra8_touch_cal_read_raw_fn_t`).
pub const ReadRawFn = *const fn (ctx: ?*anyopaque, out_raw: *Point) callconv(.c) RawErr;

/// Everything `ra8_touch_cal_run` needs (`ra8_touch_cal_run_cfg_t`).
pub const RunConfig = extern struct {
    screen_width: u16 = 0,
    screen_height: u16 = 0,
    inset_px: u16 = 0,
    draw_target: ?DrawTargetFn = null,
    draw_ctx: ?*anyopaque = null,
    read_raw: ?ReadRawFn = null,
    read_ctx: ?*anyopaque = null,
};

comptime {
    if (@sizeOf(CalError) != 2) @compileError("ra8_err_t width");
    if (@intFromEnum(CalError.ok) != 0) @compileError("k_ra8_ok value");
    if (@intFromEnum(CalError.invalid_arg) != 0x103) @compileError("k_ra8_err_invalid_arg value");
    if (@intFromEnum(CalError.invalid_size) != 0x105) @compileError("k_ra8_err_invalid_size value");
    if (@intFromEnum(CalError.hw_error) != 0x204) @compileError("k_ra8_err_hw_error value");
    if (@intFromEnum(CalError.crc_mismatch) != 0x405) @compileError("k_ra8_err_crc_mismatch value");
    if (@intFromEnum(CalError.null_ptr) != 0x504) @compileError("k_ra8_err_null_ptr value");

    // Pointer-width aware, so the same asserts hold for the 64-bit host build
    // and the 32-bit Arm cross build. Three `uint16_t` fields round up to the
    // pointer alignment before the first seam, which is 8 either way.
    const word = @sizeOf(usize);
    if (@offsetOf(RunConfig, "screen_height") != 2) @compileError("run_cfg screen_height offset");
    if (@offsetOf(RunConfig, "inset_px") != 4) @compileError("run_cfg inset_px offset");
    if (@offsetOf(RunConfig, "draw_target") != 8) @compileError("run_cfg draw_target offset");
    if (@offsetOf(RunConfig, "draw_ctx") != 8 + word) @compileError("run_cfg draw_ctx offset");
    if (@offsetOf(RunConfig, "read_raw") != 8 + (word * 2)) @compileError("run_cfg read_raw offset");
    if (@offsetOf(RunConfig, "read_ctx") != 8 + (word * 3)) @compileError("run_cfg read_ctx offset");
    if (@sizeOf(RunConfig) != 8 + (word * 4)) @compileError("ra8_touch_cal_run_cfg_t size");
}

fn err(code: CalError) RawErr {
    return @intFromEnum(code);
}

/// `ra8_touch_cal_compute`: fit the affine transform from N sample pairs.
///
/// Pure math, no hardware, and the workhorse the unit suites drive.
pub export fn ra8_touch_cal_compute(
    raw: ?[*]const Point,
    screen: ?[*]const Point,
    n: u8,
    out_mtx: ?*Matrix,
) callconv(.c) RawErr {
    const raw_points = raw orelse return err(.null_ptr);
    const screen_points = screen orelse return err(.null_ptr);
    const out = out_mtx orelse return err(.null_ptr);

    // Bound the spans before slicing: `n` is validated inside `compute`, but a
    // slice of an unvalidated length would be built first.
    if (n < implementation.min_targets or n > implementation.max_targets) {
        return err(.invalid_arg);
    }

    const fitted = implementation.compute(raw_points[0..n], screen_points[0..n], n) catch |fit_err| switch (fit_err) {
        // Both refusals are the same story to a caller: the samples cannot
        // produce a transform. The C collapsed them the same way.
        error.SampleCountOutOfRange, error.SingularSystem => return err(.invalid_arg),
    };

    out.* = fitted;
    return err(.ok);
}

/// `ra8_touch_cal_run`: drive the on-screen sequence and fit the result.
///
/// Five targets, painted in a fixed order, each followed by one blocking
/// read. Any seam failure is reported as `hw_error` rather than passed
/// through, because the caller's recovery is the same whichever shim broke.
pub export fn ra8_touch_cal_run(cfg: ?*const RunConfig, out_matrix: ?*Matrix) callconv(.c) RawErr {
    const config = cfg orelse return err(.null_ptr);
    const out = out_matrix orelse return err(.null_ptr);

    const draw = config.draw_target orelse return err(.null_ptr);
    const read = config.read_raw orelse return err(.null_ptr);

    if (config.screen_width == 0 or config.screen_height == 0) return err(.invalid_arg);
    if (!implementation.insetFits(config.screen_width, config.screen_height, config.inset_px)) {
        return err(.invalid_arg);
    }

    const targets = implementation.targetsFor(config.screen_width, config.screen_height, config.inset_px);
    var samples: [implementation.n_targets]Point = @splat(.{});

    for (targets, 0..) |target, i| {
        if (draw(config.draw_ctx, target) != err(.ok)) return err(.hw_error);
        if (read(config.read_ctx, &samples[i]) != err(.ok)) return err(.hw_error);
    }

    return ra8_touch_cal_compute(&samples, &targets, implementation.n_targets, out);
}

/// `ra8_touch_cal_apply`: map one raw sample onto the panel.
pub export fn ra8_touch_cal_apply(
    raw: Point,
    matrix: ?*const Matrix,
    screen_width: u16,
    screen_height: u16,
    out_screen: ?*Point,
) callconv(.c) RawErr {
    const transform = matrix orelse return err(.null_ptr);
    const out = out_screen orelse return err(.null_ptr);
    if (screen_width == 0 or screen_height == 0) return err(.invalid_arg);

    out.* = implementation.applyMatrix(raw, transform.*, screen_width, screen_height);
    return err(.ok);
}

/// `ra8_touch_cal_save`: serialise a matrix into the fixed-size blob.
///
/// The blob is written but never stored here: the caller owns the flash page,
/// so this module keeps no opinion about where a calibration lives.
pub export fn ra8_touch_cal_save(
    matrix: ?*const Matrix,
    dst: ?[*]u8,
    dst_size: usize,
) callconv(.c) RawErr {
    const transform = matrix orelse return err(.null_ptr);
    const destination = dst orelse return err(.null_ptr);

    implementation.serialize(transform.*, destination[0..dst_size]) catch |encode_err| switch (encode_err) {
        error.ShortBuffer => return err(.invalid_size),
    };
    return err(.ok);
}

/// `ra8_touch_cal_load`: decode a blob back into a matrix.
///
/// On any refusal `out_matrix` is left exactly as the caller had it, so a
/// failed load cannot half-replace a calibration that was already good.
pub export fn ra8_touch_cal_load(
    src: ?[*]const u8,
    src_size: usize,
    out_matrix: ?*Matrix,
) callconv(.c) RawErr {
    const source = src orelse return err(.null_ptr);
    const out = out_matrix orelse return err(.null_ptr);

    const decoded = implementation.deserialize(source[0..src_size]) catch |decode_err| switch (decode_err) {
        error.ShortBuffer => return err(.invalid_size),
        error.BadHeader => return err(.invalid_arg),
        error.CrcMismatch => return err(.crc_mismatch),
    };

    out.* = decoded;
    return err(.ok);
}
