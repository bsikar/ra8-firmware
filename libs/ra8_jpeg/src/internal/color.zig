//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! BT.601 RGB to YCbCr conversion for the baseline encoder, in the
//! same Q16 fixed point as the decoder's inverse transform.

const spec = @import("spec");

/// Q16 forward BT.601 coefficients.
pub const Coefficient = struct {
    pub const y_r: i32 = 19595; // 0.29900 * 65536
    pub const y_g: i32 = 38470; // 0.58700 * 65536
    pub const y_b: i32 = 7471; //  0.11400 * 65536
    pub const cb_r: i32 = -11059;
    pub const cb_g: i32 = -21709;
    pub const cb_b: i32 = 32768;
    pub const cr_r: i32 = 32768;
    pub const cr_g: i32 = -27439;
    pub const cr_b: i32 = -5329;

    pub const shift: u5 = 16;
};

/// Convert one packed RGB888 row into Y, Cb and Cr rows. The three output
/// slices set the pixel count, so they must be the same length and `rgb` must
/// hold three bytes per pixel. Chroma carries the +128 level offset.
pub fn rowToYcc(rgb: []const u8, y: []i32, cb: []i32, cr: []i32) void {
    for (y, cb, cr, 0..) |*luma, *blue, *red, i| {
        const px = i * spec.Limits.rgb_channels;
        const r: i32 = rgb[px];
        const g: i32 = rgb[px + 1];
        const b: i32 = rgb[px + 2];

        luma.* = ((Coefficient.y_r * r) + (Coefficient.y_g * g) + (Coefficient.y_b * b)) >>
            Coefficient.shift;
        blue.* = (((Coefficient.cb_r * r) + (Coefficient.cb_g * g) + (Coefficient.cb_b * b)) >>
            Coefficient.shift) + spec.Block.level_offset;
        red.* = (((Coefficient.cr_r * r) + (Coefficient.cr_g * g) + (Coefficient.cr_b * b)) >>
            Coefficient.shift) + spec.Block.level_offset;
    }
}
