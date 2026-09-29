//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Per-panel gray-level tone curve for `ra8_gfx` (#479): the measured gray8
//! each of the panel's sixteen levels renders, and the prepared per-sample
//! map the dither quantises against. Pure integer arithmetic over caller
//! memory, no binding and no logging, so the same input produces the same
//! bytes on host, emulator and silicon.

const std = @import("std");
/// The library core, re-exported so a consumer of this module reaches the
/// same error codes without importing the file twice.
pub const core = @import("root.zig");

/// `ra8_gfx_tone_const_t`.
pub const tone = struct {
    pub const knots: u16 = 16;
    pub const last_knot: u16 = 15;
    pub const domain: u16 = 256;
    pub const white: u8 = 255;
    pub const scale: u32 = 256;
};

/// `ra8_gfx_tone_lut_t`: the gray8 tone each panel level renders, in level
/// order. This is the shape a bench measurement produces.
pub const Lut = extern struct {
    level_gray8: [tone.knots]u8,
};

/// `ra8_gfx_tone_map_t`: one prepared entry per gray8 sample. `level` is the
/// knot at or below the sample and `up_threshold` is the exclusive blue-noise
/// threshold above which it takes the next level.
pub const Map = extern struct {
    level: [tone.domain]u8,
    up_threshold: [tone.domain]u16,
};

comptime {
    std.debug.assert(@sizeOf(Lut) == tone.knots);
    std.debug.assert(@offsetOf(Map, "level") == 0);
    std.debug.assert(@offsetOf(Map, "up_threshold") == tone.domain);
    std.debug.assert(tone.last_knot == tone.knots - 1);
}

/// The committed uncalibrated curve: level n renders `n * 17`, the `(n << 4) | n`
/// expansion the rest of the reader uses. A default, never a measurement.
pub const nominal = Lut{
    .level_gray8 = .{
        0,   17,  34,  51,  68,  85,  102, 119,
        136, 153, 170, 187, 204, 221, 238, 255,
    },
};

/// The knot at or below `gray8`: walks down from the white knot and floors at
/// black, so a curve whose first knot sits above the sample still brackets.
pub fn bracket(lut: *const Lut, gray8: u8) u8 {
    var n: u8 = @intCast(tone.last_knot);
    while (n > 0 and lut.level_gray8[n] > gray8) {
        n -= 1;
    }
    return n;
}

/// The curve contract: black knot 0, white knot 255, strictly increasing.
/// Strict monotonicity keeps every level distinguishable and every interval
/// width non-zero; the pinned endpoints keep the curve a map of the whole
/// 0..255 source domain.
pub fn validateLut(lut: *const Lut) u16 {
    if (lut.level_gray8[0] != 0) {
        return core.err.range_check_failed;
    }
    if (lut.level_gray8[tone.last_knot] != tone.white) {
        return core.err.range_check_failed;
    }
    var n: usize = 1;
    while (n < tone.knots) : (n += 1) {
        if (lut.level_gray8[n] <= lut.level_gray8[n - 1]) {
            return core.err.range_check_failed;
        }
    }
    return core.err.ok;
}

/// `ceil(rem * 256 / span)`, the exact integer form of the unbiased test
/// `thr * span < rem * 256`, so `thr < up_threshold` is that comparison and
/// the round-up probability over a uniform mask is exactly `rem / span`.
pub fn upThreshold(rem: u32, span: u32) u16 {
    return @intCast(((rem * tone.scale) + span - 1) / span);
}

/// Turn a validated curve into the per-sample map. The white knot has no
/// interval above it, so it never rounds up and no seventeenth level can be
/// produced.
pub fn prepareMap(lut: *const Lut, out: *Map) void {
    var v: u32 = 0;
    while (v < tone.domain) : (v += 1) {
        const n = bracket(lut, @intCast(v));
        out.level[v] = n;
        if (n == tone.last_knot) {
            out.up_threshold[v] = 0;
            continue;
        }
        const lo: u32 = lut.level_gray8[n];
        const hi: u32 = lut.level_gray8[n + 1];
        out.up_threshold[v] = upThreshold(v - lo, hi - lo);
    }
}

/// The prepared quantise rule: the sample's knot, plus one when the pixel's
/// blue-noise threshold falls below the sample's exclusive round-up threshold.
pub fn quantise(map: *const Map, gray8: u8, thr: u8) u8 {
    const level = map.level[gray8];
    return if (@as(u16, thr) < map.up_threshold[gray8]) level + 1 else level;
}
