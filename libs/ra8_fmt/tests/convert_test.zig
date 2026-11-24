//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for `libs/ra8_fmt/src/internal/convert.zig`.

const std = @import("std");
const c_abi = @import("c_abi");
const convert = @import("convert");
const mocks = @import("mocks");

fn makeConvertDeps() c_abi.Deps {
    var d = mocks.defaultStubDeps();
    const H = struct {
        fn produce(cfg: *const c_abi.JofProduceCfg, info: *c_abi.JofInfo) callconv(.c) c_abi.Ra8Err {
            var pulled: [128]u8 = undefined;
            var got: usize = 0;
            const rc = cfg.pull.?(cfg.pull_ctx, &pulled, pulled.len, &got);
            if (rc != c_abi.k_ra8_ok) return rc;
            const rc_sink = cfg.sink.?(cfg.sink_ctx, pulled[0..@min(got, 16)].ptr, @min(got, 16));
            if (rc_sink != c_abi.k_ra8_ok) return rc_sink;
            info.* = .{
                .width = cfg.max_width,
                .height = cfg.max_height,
                .tile_w = cfg.tile_w,
                .tile_h = cfg.tile_h,
                .tile_cols = 1,
                .tile_rows = 2,
                .bpp = 3,
                .codec = cfg.codec,
                .tile_count = 2,
                .index_off = 32,
                .total_size = 123,
            };
            return c_abi.k_ra8_ok;
        }
    };
    d.jof_produce = H.produce;
    return d;
}

test "jofConvertRequirements probes JPEG APP0, fill 0xFF, and SOF0 geometry" {
    const jpeg = [_]u8{
        0xFF, 0xD8,
        0xFF, 0xFF,
        0xE0, 0x00,
        0x04, 0x4A,
        0x46, 0xFF,
        0xC0, 0x00,
        0x11, 0x08,
        0x01, 0x40,
        0x00, 0xC8,
        0x03, 0x01,
        0x11, 0x00,
        0x02, 0x11,
        0x00, 0x03,
        0x11, 0x00,
    };
    var src_state = mocks.MemSource{ .data = &jpeg };
    const src = src_state.asSource(false);
    var req: c_abi.JofConvertRequirements = undefined;

    const rc = convert.jofConvertRequirements(comptime makeConvertDeps(), &src, &req);
    try std.testing.expectEqual(c_abi.k_ra8_ok, rc);
    try std.testing.expectEqual(@as(u16, 200), req.width);
    try std.testing.expectEqual(@as(u16, 320), req.height);
    try std.testing.expectEqual(@as(u16, 200), req.tile_width);
    try std.testing.expectEqual(@as(u16, 256), req.tile_height);
    try std.testing.expectEqual(@as(u32, 0), req.webp_work_bytes);
}

test "jofConvertRequirements rejects progressive JPEG and short or zero-dim PNG" {
    const prog_jpeg = [_]u8{
        0xFF, 0xD8,
        0xFF, 0xC2,
        0x00, 0x08,
        0x08, 0x00,
        0x10, 0x00,
        0x10, 0x03,
    };
    var p_state = mocks.MemSource{ .data = &prog_jpeg };
    const p_src = p_state.asSource(false);
    var req: c_abi.JofConvertRequirements = undefined;
    try std.testing.expectEqual(
        c_abi.k_ra8_err_not_supported,
        convert.jofConvertRequirements(comptime makeConvertDeps(), &p_src, &req),
    );

    const bad_png = [_]u8{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A } ++
        [_]u8{ 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 0, 0, 0, 0, 8, 8, 2, 0, 0, 0 };
    var png_state = mocks.MemSource{ .data = &bad_png };
    const png_src = png_state.asSource(false);
    try std.testing.expectEqual(
        c_abi.k_ra8_err_invalid_size,
        convert.jofConvertRequirements(comptime makeConvertDeps(), &png_src, &req),
    );
}

test "jofConvertRequirements sizes WebP whole-frame arena and handles WebP probe" {
    const webp = [_]u8{ 'R', 'I', 'F', 'F', 20, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' ' };
    var src_state = mocks.MemSource{ .data = &webp };
    const src = src_state.asSource(false);
    var req: c_abi.JofConvertRequirements = undefined;

    const rc = convert.jofConvertRequirements(comptime makeConvertDeps(), &src, &req);
    try std.testing.expectEqual(c_abi.k_ra8_ok, rc);
    try std.testing.expectEqual(@as(u16, 64), req.width);
    try std.testing.expectEqual(@as(u16, 32), req.height);
    try std.testing.expectEqual(@as(u16, 32), req.tile_height);
    try std.testing.expect(req.webp_work_bytes > 0);
}

test "jofConvertStream publishes committed artifact and aborts on failures" {
    const png = [_]u8{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A } ++
        [_]u8{ 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 10, 0, 0, 0, 20, 8, 2, 0, 0, 0, 0 };
    var src_state = mocks.MemSource{ .data = &png };
    const src = src_state.asSource(false);
    var req: c_abi.JofConvertRequirements = undefined;
    try std.testing.expectEqual(c_abi.k_ra8_ok, convert.jofConvertRequirements(comptime makeConvertDeps(), &src, &req));

    var work: [4096]u8 = undefined;
    const ws = c_abi.JofConvertWorkspace{
        .work = &work,
        .work_cap = work.len,
        .webp_work = null,
        .webp_work_cap = 0,
    };
    var tx_state = mocks.MemTx{};
    var tx = tx_state.asTx();
    var sink_state = mocks.MemSink{};
    const sink = sink_state.asSink();

    const rc = convert.jofConvertStream(comptime makeConvertDeps(), &src, &req, &ws, &tx, &sink, "out.jof");
    try std.testing.expectEqual(c_abi.k_ra8_ok, rc);
    try std.testing.expect(tx_state.committed);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "convert: 10x20 bpp=3 band=20 tiles=2 -> out.jof (123 bytes)\n") != null);

    var tx_fail_commit = mocks.MemTx{ .commit_rc = c_abi.k_ra8_err_no_mem };
    var tx2 = tx_fail_commit.asTx();
    try std.testing.expectEqual(
        c_abi.k_ra8_err_no_mem,
        convert.jofConvertStream(comptime makeConvertDeps(), &src, &req, &ws, &tx2, &sink, "out.jof"),
    );
    try std.testing.expect(tx_fail_commit.aborted);

    var tx_short = mocks.MemTx{};
    var tx3 = tx_short.asTx();
    const bad_ws = c_abi.JofConvertWorkspace{ .work = &work, .work_cap = 1, .webp_work = null, .webp_work_cap = 0 };
    try std.testing.expectEqual(
        c_abi.k_ra8_err_invalid_size,
        convert.jofConvertStream(comptime makeConvertDeps(), &src, &req, &bad_ws, &tx3, &sink, "out.jof"),
    );
    try std.testing.expect(tx_short.aborted);
}
