//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `inc/ra8_camera_stream.h`. Holds the library's only
//! exported symbol and its only two link-time seams, so `internal/root.zig`
//! stays pure.
//!
//! The library still names no codec and no sink: `ra8_camera_codec_encode`
//! comes from the `ra8_camera` archive and `ra8_io_stream_write` from the
//! `ra8_io` objects inside `ra8_core_hal`, exactly as they did for the C.

const core = @import("internal/root.zig");

pub const Buffer = core.Buffer;
pub const Frame = core.Frame;

/// `ra8_camera_codec_encode`, from the `ra8_camera` archive.
extern fn ra8_camera_codec_encode(
    codec: ?*anyopaque,
    input: ?*const Frame,
    output_buffer: ?*const Buffer,
    out_frame: ?*Frame,
) callconv(.c) u16;

/// `ra8_io_stream_write`, from the `ra8_io` objects inside `ra8_core_hal`.
extern fn ra8_io_stream_write(
    stream: ?*anyopaque,
    buf: ?[*]const u8,
    len: u32,
    out_written: ?*u32,
) callconv(.c) u16;

/// `ra8_camera_codec_encode_to_stream`: encode one frame, then write the
/// whole result to the sink in a single bounded operation.
///
/// Guard order is the contract. The null-sink check runs BEFORE the counter
/// is zeroed, so a null sink leaves the caller's counter untouched. `codec`,
/// `input` and `output_buffer` are deliberately not checked here: the camera
/// facade validates them and its codes are forwarded verbatim.
///
/// A zero-byte encoded frame is forwarded as a zero-length request carrying
/// whatever pointer the codec published, null included. The bridge
/// substitutes nothing and lets the sink judge it.
pub export fn ra8_camera_codec_encode_to_stream(
    codec: ?*anyopaque,
    input: ?*const Frame,
    output_buffer: ?*const Buffer,
    stream: ?*anyopaque,
    out_written: ?*u32,
) callconv(.c) u16 {
    if (stream == null) return core.err.null_ptr;
    if (out_written) |slot| slot.* = 0;

    var encoded: Frame = .{};
    const codec_err = ra8_camera_codec_encode(codec, input, output_buffer, &encoded);
    if (codec_err != core.err.ok) return codec_err;

    var written: u32 = 0;
    const stream_err = ra8_io_stream_write(stream, encoded.data, encoded.bytes, &written);
    if (out_written) |slot| slot.* = written;
    return core.writeOutcome(stream_err, written, encoded.bytes);
}
