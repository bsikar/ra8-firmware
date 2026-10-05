//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the MIPI DSI-2 video-mode surface (ra8_mipi_dsi_api.h),
//! RA8FW-652, replacing that block of ra8_mipi_dsi_dispatch.c. Struct
//! layouts are asserted against the C sizes at comptime; the bounded
//! status poll is the shared priv_ra8_mipi_dsi_internal_wait_eq.

const std = @import("std");
const common = @import("abi_common.zig");
const vd = @import("internal/mipi_dsi_video.zig");

const tag = "MIPI_DSI";
const base_addr: usize = 0x40346000;

comptime {
    std.debug.assert(@sizeOf(vd.VideoCfg) == 26);
    std.debug.assert(@offsetOf(vd.VideoCfg, "hsync_active_high") == 7);
    std.debug.assert(@offsetOf(vd.VideoCfg, "vertical_sync_lines") == 8);
    std.debug.assert(@offsetOf(vd.VideoCfg, "video_mode_delay") == 24);
    std.debug.assert(@sizeOf(vd.VideoTiming) == 16);
}

extern fn priv_ra8_mipi_dsi_internal_wait_eq(reg: *const volatile u32, mask: u32, expect: u32) u16;

fn regPtr(off: u16) *volatile u32 {
    return @ptrFromInt(base_addr + off);
}

const Dsi = struct {
    pub fn write32(_: Dsi, off: u16, value: u32) void {
        regPtr(off).* = value;
    }
    pub fn waitEq(_: Dsi, off: u16, mask: u32, expect: u32) u16 {
        return priv_ra8_mipi_dsi_internal_wait_eq(regPtr(off), mask, expect);
    }
    /// RA8_CHECK_NULL_PTR's single log line.
    pub fn err(_: Dsi, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

const dsi = Dsi{};

export fn ra8_mipi_dsi_video_configure(vcfg: ?*const vd.VideoCfg) u16 {
    return vd.configure(dsi, vcfg);
}

export fn ra8_mipi_dsi_video_start(vcfg: ?*const vd.VideoCfg) u16 {
    return vd.start(dsi, vcfg);
}

export fn ra8_mipi_dsi_video_stop() u16 {
    return vd.stop(dsi);
}

export fn ra8_mipi_dsi_set_video_timing(timing: ?*const vd.VideoTiming) u16 {
    return vd.setVideoTiming(dsi, timing);
}
