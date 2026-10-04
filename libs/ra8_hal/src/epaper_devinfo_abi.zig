//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI export for ra8_epaper_decode_dev_info (internal/epaper_devinfo.zig,
//! RA8FW-548). Built as its own object in libra8_hal.a (RA8FW-542) so an
//! image links only the units it calls.

const common = @import("abi_common.zig");
const devinfo = @import("internal/epaper_devinfo.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_error = common.ra8_log_emit_error;

const tag = "EPAPER";

/// `ra8_err_t ra8_epaper_decode_dev_info(const uint16_t* words, size_t count,
/// ra8_epaper_dev_info_t* out_info)`.
export fn ra8_epaper_decode_dev_info(words: ?[*]const u16, count: usize, out_info: ?*devinfo.DevInfo) u16 {
    const src = words orelse {
        ra8_log_emit_error(tag, "decode_dev_info: words null");
        return k_ra8_err_null_ptr;
    };
    const out = out_info orelse {
        ra8_log_emit_error(tag, "decode_dev_info: out null");
        return k_ra8_err_null_ptr;
    };
    if (count < devinfo.word_count) {
        ra8_log_emit_error(tag, "decode_dev_info: short response");
        return k_ra8_err_invalid_arg;
    }
    out.* = devinfo.decode(src[0..devinfo.word_count]);
    return k_ra8_ok;
}
