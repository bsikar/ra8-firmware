//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the baseline decoder (#2799).
//!
//! The decoder is Zig throughout; this is the only place its C callers see.
//! Three entry points carry the public API (`inc/ra8_jpeg_sw.h`), and three
//! `priv_` primitives stay exported because `tests/graphics/src/
//! test_ra8_jpeg_sw_cov.c` drives them directly through the private header.
//!
//! Nothing here computes. Every body translates arguments into the internal
//! modules and their results back into `ra8_err_t`.

const bitreader = @import("bitreader");
const dec_ctx = @import("dec_ctx");
const dims = @import("dims");
const huffdec = @import("huffdec");
const stream = @import("stream");
const whole = @import("whole");

/// `ra8_err_t` values this membrane returns.
pub const Error = enum(u16) {
    ok = 0,
    invalid_size = 0x105,
    not_supported = 0x107,
    protocol_error = 0x406,
    null_ptr = 0x504,
};

/// Component tag on this unit's log lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "JPEG_SW";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// A failed read reports -1, which is outside the 0..255 symbol range and
/// outside any bit value, so it cannot collide with a real result.
const read_failed: i32 = -1;

/// Map a parse failure onto its `ra8_err_t`.
fn code(err: dec_ctx.Error) u16 {
    return @intFromEnum(switch (err) {
        dec_ctx.Error.InvalidSize => Error.invalid_size,
        dec_ctx.Error.Unsupported => Error.not_supported,
        dec_ctx.Error.Protocol => Error.protocol_error,
    });
}

/// Report a null argument the way the C `RA8_CHECK_NULL_PTR` macro did, with
/// the same message text.
fn nullPtr(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return @intFromEnum(Error.null_ptr);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Parse state for the whole-buffer path. Static for the same reason the C
/// kept it static: the context is several kilobytes and this path runs on
/// targets where that does not belong on the stack. Documented as neither
/// thread-safe nor re-entrant in `inc/ra8_jpeg_sw.h`.
var whole_ctx: dec_ctx.Ctx = .{};

/// Decode a complete in-memory JPEG into RGB888.
pub export fn ra8_jpeg_sw_decode(
    jpeg_buf: ?[*]const u8,
    jpeg_len: u32,
    out_buf: ?[*]u8,
    out_buf_len: u32,
    out_w: ?*u16,
    out_h: ?*u16,
) u16 {
    const src = jpeg_buf orelse return nullPtr("jpeg_buf is NULL");
    const dst = out_buf orelse return nullPtr("out_buf is NULL");
    const width = out_w orelse return nullPtr("out_w is NULL");
    const height = out_h orelse return nullPtr("out_h is NULL");

    if (jpeg_len < whole.min_stream_len) return @intFromEnum(Error.invalid_size);

    const found = whole.decode(
        &whole_ctx,
        src[0..jpeg_len],
        dst[0..out_buf_len],
    ) catch |err| return code(err);

    width.* = found.width;
    height.* = found.height;
    return @intFromEnum(Error.ok);
}

/// Striped-decode state. Static for the same reason as `whole_ctx`.
var stripe_state: stream.State = undefined;

/// Decode a JPEG pulled from a byte source, emitting one MCU row at a time.
pub export fn ra8_jpeg_sw_decode_stripes(
    pull: ?stream.PullFn,
    pull_ctx: ?*anyopaque,
    window: ?[*]u8,
    window_cap: u32,
    on_geom: ?stream.GeomFn,
    on_rows: ?stream.RowsFn,
    cb_ctx: ?*anyopaque,
) u16 {
    const source = pull orelse return nullPtr("pull must not be nullptr");
    const buffer = window orelse return nullPtr("window must not be nullptr");
    const geometry = on_geom orelse return nullPtr("on_geom must not be nullptr");
    const sink = on_rows orelse return nullPtr("on_rows must not be nullptr");

    if (window_cap < stream.Limit.min_window) return @intFromEnum(Error.invalid_size);

    stripe_state = .{
        .pull = source,
        .pull_ctx = pull_ctx,
        .window = buffer[0..window_cap],
        .on_rows = sink,
        .cb_ctx = cb_ctx,
    };

    stream.decode(&stripe_state, geometry) catch |err| switch (err) {
        // A callback's own error code passes through unchanged.
        stream.Error.Callback => return stripe_state.callback_err,
        else => |parse_err| return code(@errorCast(parse_err)),
    };
    return @intFromEnum(Error.ok);
}

/// Read a JPEG's pixel dimensions without decoding it. Re-entrant.
pub export fn ra8_jpeg_sw_get_dimensions(
    jpeg_buf: ?[*]const u8,
    jpeg_len: u32,
    out_w: ?*u16,
    out_h: ?*u16,
) u16 {
    const buf = jpeg_buf orelse return nullPtr("jpeg_buf is NULL");
    const width = out_w orelse return nullPtr("out_w is NULL");
    const height = out_h orelse return nullPtr("out_h is NULL");

    if (jpeg_len < dims.min_stream_len) return @intFromEnum(Error.invalid_size);

    const found = dims.probe(buf[0..jpeg_len]) catch |err| switch (err) {
        dims.Error.Unsupported => return @intFromEnum(Error.not_supported),
        dims.Error.Protocol => return @intFromEnum(Error.protocol_error),
    };

    width.* = found.width;
    height.* = found.height;
    return @intFromEnum(Error.ok);
}

// ---------------------------------------------------------------------------
// Primitives the external coverage suite drives directly
// ---------------------------------------------------------------------------

/// Take `n` bits from the entropy stream. -1 when the stream is exhausted.
pub export fn priv_jpeg_sw_br_get_bits(br: *bitreader.BitReader, n: u8) i32 {
    const bits = br.getBits(n) orelse return read_failed;
    return @intCast(bits);
}

/// Derive the canonical codes and lookup for a table whose BITS and VALS a
/// DHT segment has already filled in.
pub export fn priv_jpeg_sw_htab_build(h: *huffdec.Table) void {
    h.build();
}

/// Read one Huffman symbol. -1 on a malformed or truncated code.
pub export fn priv_jpeg_sw_htab_decode(
    br: *bitreader.BitReader,
    h: *const huffdec.Table,
) i32 {
    const symbol = h.decode(br) orelse return read_failed;
    return @intCast(symbol);
}
