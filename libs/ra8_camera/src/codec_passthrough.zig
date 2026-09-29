//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zero-copy JPEG passthrough codec: accepts only `k_ra8_camera_format_jpeg`
//! and aliases the validated input view, never touching the output buffer.

pub const abi = @import("abi_types.zig");

const core = abi.core;
const err = abi.err;

/// Codec vtable row: return a JPEG input view unchanged.
///
/// The NULL guards stay even though the facade filters both arguments: the
/// host suite dispatches through the bound vtable directly, which is the only
/// route to them.
fn encode(
    ctx: ?*anyopaque,
    input: ?*const core.Frame,
    output_buffer: ?*const core.Buffer,
    out_frame: ?*core.Frame,
) callconv(.c) u16 {
    _ = ctx;
    _ = output_buffer;
    const source_frame = input orelse return err.null_ptr;
    const out = out_frame orelse return err.null_ptr;
    if (source_frame.format != core.format.jpeg) {
        return err.not_supported;
    }
    out.* = source_frame.*;
    return err.ok;
}

/// Stateless passthrough codec vtable.
pub const iface: abi.CodecIface = .{ .encode = encode };

pub export fn ra8_camera_codec_passthrough_init(codec: ?*abi.Codec) callconv(.c) u16 {
    const handle = codec orelse return err.null_ptr;
    handle.iface = &iface;
    handle.ctx = null;
    return err.ok;
}
