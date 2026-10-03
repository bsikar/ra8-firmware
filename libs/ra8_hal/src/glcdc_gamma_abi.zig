//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_glcdc_set_gamma / ra8_glcdc_gamma_enable
//! (internal/glcdc_gamma.zig, RA8FW-545). Built as its own object in
//! libra8_hal.a (RA8FW-542) so an image links only the units it calls.

const common = @import("abi_common.zig");
const gamma = @import("internal/glcdc_gamma.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_info_val = common.ra8_log_emit_info_val;
const ra8_log_emit_error = common.ra8_log_emit_error;

const tag = "GLCDC";
const window: gamma.Window = .{ .base = gamma.base };

/// `ra8_err_t ra8_glcdc_set_gamma(ra8_glcdc_color_channel_t, const uint16_t* gain,
/// const uint16_t* threshold, uint8_t count)`.
export fn ra8_glcdc_set_gamma(channel: u8, gain: ?*const [gamma.lut_depth]u16, threshold: ?*const [gamma.lut_depth]u16, count: u8) u16 {
    const gains = gain orelse {
        ra8_log_emit_error(tag, "gain must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    const thresholds = threshold orelse {
        ra8_log_emit_error(tag, "threshold must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    gamma.validate(channel, count) catch return k_ra8_err_invalid_arg;
    gamma.writeTables(window, channel, gains, thresholds);
    ra8_log_emit_info_val(tag, "set_gamma channel", channel);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_glcdc_gamma_enable(bool enable)`.
export fn ra8_glcdc_gamma_enable(enable: bool) u16 {
    gamma.setEnable(window, enable);
    ra8_log_emit_info_val(tag, "gamma_enable", @intFromBool(enable));
    return k_ra8_ok;
}
