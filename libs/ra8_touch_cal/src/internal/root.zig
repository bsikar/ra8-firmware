//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure calibration math for `ra8_touch_cal`: the weighted least-squares
//! affine fit (Fang & Chang, Analog Dialogue 41-08), the 3x3 Cramer solve it
//! rests on, the pixel mapping with its clip, and the 36-byte `'TCAL'` storage
//! codec. Nothing here touches an injected seam or a hardware header, so every
//! branch is reachable from a plain unit test.
//!
//! The arithmetic is deliberately `f32` end to end, exactly as the C was: the
//! Cortex-M85 has a hardware FPU, and a Q15.16 rewrite would cost two extra
//! multiplies per mapped pixel for worse conditioning on the normal equations.

const std = @import("std");

/// Target points the guided run paints (`k_ra8_touch_cal_n_targets`).
pub const n_targets: u8 = 5;
/// Fewest sample pairs that still pin a unique solve (`..._min_targets`).
pub const min_targets: u8 = 3;
/// Most sample pairs the fit accepts (`k_ra8_touch_cal_max_targets`).
pub const max_targets: u8 = 5;
/// Serialised blob size in bytes (`k_ra8_touch_cal_blob_size`).
pub const blob_size: usize = 36;
/// On-storage format version (`k_ra8_touch_cal_storage_version`).
pub const storage_version: u8 = 1;
/// Affine coefficients in a matrix, and floats in the serialised blob.
pub const n_coeffs: usize = 6;

/// Byte offsets inside the serialised blob (`ra8_touch_cal_layout_t`).
pub const offset = struct {
    pub const magic: usize = 0;
    pub const version: usize = 4;
    pub const reserved: usize = 5;
    pub const coeffs: usize = 8;
    pub const crc32: usize = 32;
};

/// `'TCAL'`, the blob's leading magic word (`ra8_touch_cal_magic_t`).
pub const magic = [4]u8{ 0x54, 0x43, 0x41, 0x4C };

/// Reserved bytes between the version and the coefficients; always zero.
pub const reserved_len: usize = 3;

/// IEEE 802.3 CRC-32 seed, shared with `ra8_epd_cal`.
pub const crc_init: u32 = 0xFFFFFFFF;
/// Reversed IEEE 802.3 polynomial.
pub const crc_poly: u32 = 0xEDB88320;

/// Floor for `|det|` below which the normal equations count as singular.
///
/// A real 1024x600 panel produces `|det|` in the 1e8..1e10 range, so 1e-3
/// only trips on genuinely collinear targets rather than on a noisy user.
pub const min_det: f32 = 1.0e-3;

/// Round-to-nearest bias applied before truncating a mapped float to a pixel.
pub const round_bias: f32 = 0.5;

/// One integer coordinate pair (`ra8_touch_cal_point_t`).
///
/// Components are `i32` so an intermediate that lands off-panel does not wrap
/// before the clip in `applyMatrix` gets to it.
pub const Point = extern struct {
    x: i32 = 0,
    y: i32 = 0,
};

/// 2-D affine transform `screen = [a b; d e] * raw + [c; f]`
/// (`ra8_touch_cal_matrix_t`).
pub const Matrix = extern struct {
    a: f32 = 0,
    b: f32 = 0,
    c: f32 = 0,
    d: f32 = 0,
    e: f32 = 0,
    f: f32 = 0,
};

/// Why a fit was refused.
pub const ComputeError = error{
    /// `n` outside [`min_targets`, `max_targets`].
    SampleCountOutOfRange,
    /// `|det|` below `min_det`: the targets are collinear.
    SingularSystem,
};

/// Why a decode was refused.
pub const DecodeError = error{
    /// Fewer than `blob_size` bytes were offered.
    ShortBuffer,
    /// Magic, version or a reserved byte is wrong.
    BadHeader,
    /// Header intact, body damaged.
    CrcMismatch,
};

/// Why an encode was refused.
pub const EncodeError = error{
    /// Destination is smaller than `blob_size`.
    ShortBuffer,
};

/// Compute the IEEE 802.3 CRC-32 of a byte span.
///
/// Bit-banged reflected-polynomial form, so the module stays independent of
/// `ra8_crc` and carries no 1 KiB lookup table for a 32-byte checksum.
pub fn crc32(data: []const u8) u32 {
    var crc: u32 = crc_init;
    for (data) |byte| {
        crc ^= byte;
        var bit: u8 = 0;
        while (bit < 8) : (bit += 1) {
            const mask: u32 = @bitCast(-%@as(i32, @intCast(crc & 1)));
            crc = (crc >> 1) ^ (crc_poly & mask);
        }
    }
    return crc ^ crc_init;
}

/// Write a 32-bit word little-endian, because the blob's byte order is a
/// property of the record and not of the compiler that wrote it.
pub fn packLe32(dst: *[4]u8, value: u32) void {
    std.mem.writeInt(u32, dst, value, .little);
}

/// Read a little-endian 32-bit word back.
pub fn unpackLe32(src: *const [4]u8) u32 {
    return std.mem.readInt(u32, src, .little);
}

/// The eleven running sums that assemble the normal equations
/// (`internal_lsq_sums_t`).
pub const Sums = struct {
    sx: f32 = 0,
    sy: f32 = 0,
    sxx: f32 = 0,
    syy: f32 = 0,
    sxy: f32 = 0,
    su: f32 = 0,
    sv: f32 = 0,
    sxu: f32 = 0,
    syu: f32 = 0,
    sxv: f32 = 0,
    syv: f32 = 0,
};

/// Accumulate the least-squares sums over paired raw and screen samples.
///
/// The two slices are walked in lockstep, so the shorter of them bounds the
/// fit; callers hand over equal-length spans.
pub fn accumulateSums(raw: []const Point, screen: []const Point) Sums {
    var s: Sums = .{};
    const count = @min(raw.len, screen.len);
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const xi: f32 = @floatFromInt(raw[i].x);
        const yi: f32 = @floatFromInt(raw[i].y);
        const ui: f32 = @floatFromInt(screen[i].x);
        const vi: f32 = @floatFromInt(screen[i].y);
        s.sx += xi;
        s.sy += yi;
        s.sxx += xi * xi;
        s.syy += yi * yi;
        s.sxy += xi * yi;
        s.su += ui;
        s.sv += vi;
        s.sxu += xi * ui;
        s.syu += yi * ui;
        s.sxv += xi * vi;
        s.syv += yi * vi;
    }
    return s;
}

/// Determinant of a row-major 3x3 matrix.
pub fn det3(a: [9]f32) f32 {
    return (a[0] * ((a[4] * a[8]) - (a[5] * a[7]))) -
        (a[1] * ((a[3] * a[8]) - (a[5] * a[6]))) +
        (a[2] * ((a[3] * a[7]) - (a[4] * a[6])));
}

/// Solve `A * x = b` by Cramer's rule, or report the system as singular.
///
/// `null` rather than an error, because the caller folds the outcome into its
/// own return path exactly as the C `ok` out-parameter did.
pub fn solve3(a: [9]f32, b: [3]f32) ?[3]f32 {
    const det = det3(a);
    const abs_det = if (det < 0) -det else det;
    if (abs_det < min_det) return null;

    const dx = (b[0] * ((a[4] * a[8]) - (a[5] * a[7]))) -
        (a[1] * ((b[1] * a[8]) - (a[5] * b[2]))) +
        (a[2] * ((b[1] * a[7]) - (a[4] * b[2])));

    const dy = (a[0] * ((b[1] * a[8]) - (a[5] * b[2]))) -
        (b[0] * ((a[3] * a[8]) - (a[5] * a[6]))) +
        (a[2] * ((a[3] * b[2]) - (b[1] * a[6])));

    const dz = (a[0] * ((a[4] * b[2]) - (b[1] * a[7]))) -
        (a[1] * ((a[3] * b[2]) - (b[1] * a[6]))) +
        (b[0] * ((a[3] * a[7]) - (a[4] * a[6])));

    return .{ dx / det, dy / det, dz / det };
}

/// Fit the affine transform that carries `raw` onto `screen`.
///
/// Both axes share one coefficient matrix and differ only in their right-hand
/// side, so the two solves are co-determined: either the shared determinant
/// clears `min_det` and both succeed, or neither does. That is why the
/// singular case is one check here rather than a compound decision, and it is
/// the same reasoning the C carried as an MC/DC deactivation note.
pub fn compute(raw: []const Point, screen: []const Point, n: u8) ComputeError!Matrix {
    if (n < min_targets or n > max_targets) return error.SampleCountOutOfRange;

    const count: usize = n;
    const s = accumulateSums(raw[0..count], screen[0..count]);
    const fn_count: f32 = @floatFromInt(n);
    const normal = [9]f32{
        s.sxx, s.sxy, s.sx,
        s.sxy, s.syy, s.sy,
        s.sx,  s.sy,  fn_count,
    };

    const sol_u = solve3(normal, .{ s.sxu, s.syu, s.su }) orelse return error.SingularSystem;
    const sol_v = solve3(normal, .{ s.sxv, s.syv, s.sv }) orelse return error.SingularSystem;

    return .{
        .a = sol_u[0],
        .b = sol_u[1],
        .c = sol_u[2],
        .d = sol_v[0],
        .e = sol_v[1],
        .f = sol_v[2],
    };
}

/// Clip `v` into `[lo, hi]` (`internal_clip32`).
pub fn clip32(v: i32, lo: i32, hi: i32) i32 {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/// Round a mapped coordinate away from zero and land it in `i32`.
///
/// The saturating bounds are the one place this is stricter than the C, which
/// left an out-of-range conversion undefined. A raw sample wild enough to
/// overflow here is clipped to the panel edge immediately afterwards, so the
/// saturation is invisible to every caller and the panel never sees a value
/// conjured out of undefined behaviour.
pub fn roundToPixel(value: f32) i32 {
    const biased = if (value >= 0) value + round_bias else value - round_bias;
    if (!(biased > @as(f32, @floatFromInt(std.math.minInt(i32))))) return std.math.minInt(i32);
    if (!(biased < @as(f32, @floatFromInt(std.math.maxInt(i32))))) return std.math.maxInt(i32);
    return @intFromFloat(biased);
}

/// Map one raw controller sample through `matrix` and clip it to the panel.
///
/// The clip is load-bearing rather than defensive: a least-squares fit maps
/// slightly off-panel raw samples past the panel edge by construction.
pub fn applyMatrix(raw: Point, matrix: Matrix, screen_width: u16, screen_height: u16) Point {
    const xf: f32 = @floatFromInt(raw.x);
    const yf: f32 = @floatFromInt(raw.y);
    const u = (matrix.a * xf) + (matrix.b * yf) + matrix.c;
    const v = (matrix.d * xf) + (matrix.e * yf) + matrix.f;

    return .{
        .x = clip32(roundToPixel(u), 0, @as(i32, screen_width) - 1),
        .y = clip32(roundToPixel(v), 0, @as(i32, screen_height) - 1),
    };
}

/// The five on-screen targets the guided run paints, in visit order:
/// top-left, top-right, bottom-right, bottom-left, centre.
pub fn targetsFor(screen_width: u16, screen_height: u16, inset_px: u16) [n_targets]Point {
    const w: i32 = screen_width;
    const h: i32 = screen_height;
    const ins: i32 = inset_px;
    return .{
        .{ .x = ins, .y = ins },
        .{ .x = w - 1 - ins, .y = ins },
        .{ .x = w - 1 - ins, .y = h - 1 - ins },
        .{ .x = ins, .y = h - 1 - ins },
        .{ .x = @divTrunc(w, 2), .y = @divTrunc(h, 2) },
    };
}

/// Does `inset_px` leave room between opposing targets on both axes?
pub fn insetFits(screen_width: u16, screen_height: u16, inset_px: u16) bool {
    const margin_total: u32 = @as(u32, inset_px) * 2;
    return margin_total < @as(u32, screen_width) and margin_total < @as(u32, screen_height);
}

/// The six coefficients in serialisation order.
pub fn coeffsOf(matrix: Matrix) [n_coeffs]f32 {
    return .{ matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f };
}

/// Rebuild a matrix from six coefficients in serialisation order.
pub fn matrixFrom(coeffs: [n_coeffs]f32) Matrix {
    return .{
        .a = coeffs[0],
        .b = coeffs[1],
        .c = coeffs[2],
        .d = coeffs[3],
        .e = coeffs[4],
        .f = coeffs[5],
    };
}

/// Pack a matrix into the 36-byte `'TCAL'` blob, CRC trailer included.
pub fn serialize(matrix: Matrix, dst: []u8) EncodeError!void {
    if (dst.len < blob_size) return error.ShortBuffer;

    const blob = dst[0..blob_size];
    @memcpy(blob[offset.magic..][0..magic.len], &magic);
    blob[offset.version] = storage_version;
    @memset(blob[offset.reserved..][0..reserved_len], 0);

    const coeffs = coeffsOf(matrix);
    for (coeffs, 0..) |coeff, i| {
        packLe32(blob[offset.coeffs + (i * 4) ..][0..4], @bitCast(coeff));
    }

    packLe32(blob[offset.crc32..][0..4], crc32(blob[0..offset.crc32]));
}

/// Decode a `'TCAL'` blob, rejecting a bad header before a bad checksum.
pub fn deserialize(src: []const u8) DecodeError!Matrix {
    if (src.len < blob_size) return error.ShortBuffer;

    const blob = src[0..blob_size];
    if (!std.mem.eql(u8, blob[offset.magic..][0..magic.len], &magic)) return error.BadHeader;
    if (blob[offset.version] != storage_version) return error.BadHeader;
    for (blob[offset.reserved..][0..reserved_len]) |byte| {
        if (byte != 0) return error.BadHeader;
    }

    const want = unpackLe32(blob[offset.crc32..][0..4]);
    const have = crc32(blob[0..offset.crc32]);
    if (want != have) return error.CrcMismatch;

    var coeffs: [n_coeffs]f32 = @splat(0);
    for (&coeffs, 0..) |*coeff, i| {
        coeff.* = @bitCast(unpackLe32(blob[offset.coeffs + (i * 4) ..][0..4]));
    }
    return matrixFrom(coeffs);
}

comptime {
    if (@sizeOf(Point) != 8) @compileError("ra8_touch_cal_point_t size");
    if (@offsetOf(Point, "y") != 4) @compileError("ra8_touch_cal_point_t y offset");
    if (@sizeOf(Matrix) != 24) @compileError("ra8_touch_cal_matrix_t size");
    if (@offsetOf(Matrix, "f") != 20) @compileError("ra8_touch_cal_matrix_t f offset");
    if (offset.crc32 + 4 != blob_size) @compileError("k_ra8_touch_cal_blob_size");
    if (offset.coeffs + (n_coeffs * 4) != offset.crc32) @compileError("coefficient span");
}
