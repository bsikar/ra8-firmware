//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_jpeg_sw` adapter for the camera codec facade. Samples RGB888 or
//! UYVY422 into the configured packed-RGB workspace, delegates baseline-JPEG
//! encoding through the unchanged `ra8_jpeg_sw_encode` seam, and translates the
//! produced byte count into a generic JPEG frame view.

const std = @import("std");
pub const abi = @import("ra8_camera_abi.zig");

const core = abi.core;
const err = abi.err;

/// `ra8_jpeg_sw_encode`, resolved from `libs/ra8_jpeg` at link time. Left as an
/// extern so the host suites keep substituting exactly what they do under C.
extern fn ra8_jpeg_sw_encode(
    rgb_buf: [*]const u8,
    width: u16,
    height: u16,
    quality: u8,
    out_buf: [*]u8,
    out_capacity: u32,
    out_bytes: *u32,
) callconv(.c) u16;

/// `ra8_camera_codec_jpeg_sw_cfg_t`.
pub const Cfg = extern struct {
    rgb_workspace: ?[*]u8 = null,
    rgb_workspace_capacity: u32 = 0,
    output_width: u16 = 0,
    output_height: u16 = 0,
    quality: u8 = 0,
};

/// `ra8_camera_codec_jpeg_sw_state_t`.
pub const State = extern struct {
    cfg: Cfg = .{},
};

const ptr_bytes = @sizeOf(usize);

comptime {
    std.debug.assert(@offsetOf(Cfg, "rgb_workspace") == 0);
    std.debug.assert(@offsetOf(Cfg, "rgb_workspace_capacity") == ptr_bytes);
    std.debug.assert(@offsetOf(Cfg, "output_width") == ptr_bytes + 4);
    std.debug.assert(@offsetOf(Cfg, "output_height") == ptr_bytes + 6);
    std.debug.assert(@offsetOf(Cfg, "quality") == ptr_bytes + 8);
    std.debug.assert(@sizeOf(Cfg) == std.mem.alignForward(usize, ptr_bytes + 9, ptr_bytes));
    std.debug.assert(@sizeOf(State) == @sizeOf(Cfg));
    std.debug.assert(@offsetOf(State, "cfg") == 0);
}

/// Codec vtable row: convert and encode one raw frame.
///
/// Every NULL guard stays: the host suite dispatches through the bound vtable
/// directly to reach the four the facade filters, and both output-buffer
/// guards answer `null_ptr` (including an empty buffer, which the C spells as
/// `null_ptr` rather than `invalid_size`).
fn encode(
    ctx: ?*anyopaque,
    input: ?*const core.Frame,
    output_buffer: ?*const core.Buffer,
    out_frame: ?*core.Frame,
) callconv(.c) u16 {
    const raw = ctx orelse return err.null_ptr;
    const source_frame = input orelse return err.null_ptr;
    const output = output_buffer orelse return err.null_ptr;
    const out = out_frame orelse return err.null_ptr;
    const destination = output.data orelse return err.null_ptr;
    if (output.capacity == 0) {
        return err.null_ptr;
    }
    if (source_frame.format != core.format.rgb888) {
        if (source_frame.format != core.format.uyvy422) {
            return err.not_supported;
        }
    }
    const state: *const State = @ptrCast(@alignCast(raw));
    const workspace = state.cfg.rgb_workspace.?;
    core.prepareRgb(
        source_frame.data.?,
        source_frame.format,
        source_frame.stride_bytes,
        source_frame.width,
        source_frame.height,
        workspace,
        state.cfg.output_width,
        state.cfg.output_height,
    );
    var produced: u32 = 0;
    const encoded = ra8_jpeg_sw_encode(
        workspace,
        state.cfg.output_width,
        state.cfg.output_height,
        state.cfg.quality,
        destination,
        output.capacity,
        &produced,
    );
    if (encoded != err.ok) {
        return encoded;
    }
    out.* = .{
        .data = destination,
        .bytes = produced,
        .stride_bytes = 0,
        .width = state.cfg.output_width,
        .height = state.cfg.output_height,
        .format = core.format.jpeg,
    };
    return err.ok;
}

/// Software JPEG codec vtable.
pub const iface: abi.CodecIface = .{ .encode = encode };

/// Configuration faults to `ra8_err_t`.
pub fn cfgErr(fault: core.JpegCfgFault) u16 {
    return switch (fault) {
        .ok => err.ok,
        .null_workspace => err.null_ptr,
        .quality_below_min,
        .quality_above_max,
        .zero_output_width,
        .zero_output_height,
        => err.invalid_arg,
        .geometry_overflows, .workspace_short => err.invalid_size,
    };
}

pub export fn ra8_camera_codec_jpeg_sw_init(
    codec: ?*abi.Codec,
    state: ?*State,
    cfg: ?*const Cfg,
) callconv(.c) u16 {
    const handle = codec orelse return err.null_ptr;
    const backend = state orelse return err.null_ptr;
    const config = cfg orelse return err.null_ptr;
    const fault = cfgErr(core.validateJpegCfg(
        config.rgb_workspace != null,
        config.quality,
        config.output_width,
        config.output_height,
        config.rgb_workspace_capacity,
    ));
    if (fault != err.ok) {
        return fault;
    }
    backend.cfg = config.*;
    handle.iface = &iface;
    handle.ctx = backend;
    return err.ok;
}
