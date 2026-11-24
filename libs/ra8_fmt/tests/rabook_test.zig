//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for `libs/ra8_fmt/src/internal/rabook.zig`.

const std = @import("std");
const c_abi = @import("c_abi");
const rabook = @import("rabook");
const mocks = @import("mocks");

fn makeRabookDeps(comptime fail_validate: bool) c_abi.Deps {
    var d = mocks.defaultStubDeps();
    const H = struct {
        fn open(
            rd: *c_abi.BookChunked,
            fr: ?c_abi.VsourceReadFn,
            fc: ?*anyopaque,
            fl: u64,
            inf: ?c_abi.BookInflateFn,
            tbl: ?[*]u64,
            tc: u32,
            st: ?[*]u8,
            sc: u32,
        ) callconv(.c) c_abi.Ra8Err {
            _ = fl;
            if (tc < 3) return c_abi.k_ra8_err_invalid_size;
            var hdr_buf: [4]u8 = undefined;
            const rc = fr.?(fc, 0, &hdr_buf, 4);
            if (rc != c_abi.k_ra8_ok) return rc;
            tbl.?[0] = 0;
            tbl.?[1] = 20;
            tbl.?[2] = 44;
            var inf_buf: [8]u8 = undefined;
            var inf_got: usize = 0;
            _ = inf.?(&hdr_buf, 4, &inf_buf, inf_buf.len, &inf_got);
            rd.* = .{
                .file_read = fr,
                .file_ctx = fc,
                .inflate_cb = inf,
                .table = tbl,
                .table_cap_entries = tc,
                .staging = st,
                .staging_cap = sc,
                .payload_off = 64,
                .inflated_total = 4096,
                .chunk_bytes = 2048,
                .chunk_count = 2,
            };
            return c_abi.k_ra8_ok;
        }
        fn validate(
            rd: *c_abi.BookChunked,
            chunk: ?[*]u8,
            chunk_cap: u32,
            scratch: ?[*]u8,
            scratch_cap: u32,
            hdr: *c_abi.BookHeader,
        ) callconv(.c) c_abi.Ra8Err {
            _ = rd;
            _ = chunk;
            _ = chunk_cap;
            _ = scratch;
            _ = scratch_cap;
            hdr.* = std.mem.zeroes(c_abi.BookHeader);
            return if (fail_validate) c_abi.k_ra8_err_crc_mismatch else c_abi.k_ra8_ok;
        }
    };
    d.book_chunked_open = H.open;
    d.book_chunked_validate_strict = H.validate;
    return d;
}

test "rabookInspectStream reports header, chunk table, and valid verdict across short reads" {
    const raw = [_]u8{ 'R', 'B', 'K', 'C' } ++ @as([120]u8, @splat(0));
    var src_state = mocks.MemSource{ .data = &raw, .max_chunk = 1 };
    const src = src_state.asSource(true);
    var table: [8]u64 = undefined;
    var compressed: [64]u8 = undefined;
    var chunk: [64]u8 = undefined;
    var scratch: [64]u8 = undefined;
    var ws = c_abi.RabookInspectWorkspace{
        .table = &table,
        .table_cap = table.len,
        .compressed = &compressed,
        .compressed_cap = compressed.len,
        .chunk = &chunk,
        .chunk_cap = chunk.len,
        .scratch = &scratch,
        .scratch_cap = scratch.len,
    };
    var sink_state = mocks.MemSink{};
    const sink = sink_state.asSink();

    const rc = rabook.rabookInspectStream(comptime makeRabookDeps(false), &src, true, &ws, &sink);
    try std.testing.expectEqual(c_abi.k_ra8_ok, rc);
    try std.testing.expectEqual(@as(u32, 1), src_state.validate_calls);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "RBKC rabook container: 124 bytes") != null);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "  entry     start        end       length") != null);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "verdict: VALID (chunk table monotonic and complete)") != null);
}

test "rabookInspectStream emits invalid verdict on validation failure or source mutation" {
    const raw = [_]u8{ 'R', 'B', 'K', 'C' } ++ @as([120]u8, @splat(0));
    var src_state = mocks.MemSource{ .data = &raw };
    const src = src_state.asSource(false);
    var table: [8]u64 = undefined;
    var compressed: [64]u8 = undefined;
    var chunk: [64]u8 = undefined;
    var scratch: [64]u8 = undefined;
    var ws = c_abi.RabookInspectWorkspace{
        .table = &table,
        .table_cap = table.len,
        .compressed = &compressed,
        .compressed_cap = compressed.len,
        .chunk = &chunk,
        .chunk_cap = chunk.len,
        .scratch = &scratch,
        .scratch_cap = scratch.len,
    };
    var sink_state = mocks.MemSink{};
    const sink = sink_state.asSink();

    const rc = rabook.rabookInspectStream(comptime makeRabookDeps(true), &src, false, &ws, &sink);
    try std.testing.expectEqual(@as(c_abi.Ra8Err, 0x405), rc);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "verdict: INVALID (strict RBKC/RABOOK1 validation failed)") != null);

    var unstable_src_state = mocks.MemSource{ .data = &raw, .validate_rc = c_abi.k_ra8_err_validation_failed };
    const unstable_src = unstable_src_state.asSource(true);
    sink_state.len = 0;
    const rc2 = rabook.rabookInspectStream(comptime makeRabookDeps(false), &unstable_src, false, &ws, &sink);
    try std.testing.expectEqual(c_abi.k_ra8_err_validation_failed, rc2);
}

test "rabookInspectStream rejects incomplete null arguments" {
    var sink_state = mocks.MemSink{};
    const sink = sink_state.asSink();
    var ws = std.mem.zeroes(c_abi.RabookInspectWorkspace);
    try std.testing.expectEqual(
        c_abi.k_ra8_err_null_ptr,
        rabook.rabookInspectStream(comptime makeRabookDeps(false), null, false, &ws, &sink),
    );
}
