//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for `libs/ra8_fmt/src/internal/inspect.zig`.

const std = @import("std");
const c_abi = @import("c_abi");
const inspect = @import("inspect");
const mocks = @import("mocks");

fn makeMockDeps(comptime mode: enum { ok, dups, invalid, too_many_tiles }) c_abi.Deps {
    var d = mocks.defaultStubDeps();
    const H = struct {
        fn audit(
            pread: ?c_abi.PreadFn,
            ctx: ?*anyopaque,
            sz: u64,
            ws: *c_abi.JofAuditWorkspace,
            out: *c_abi.JofAuditResult,
        ) callconv(.c) c_abi.Ra8Err {
            _ = pread;
            _ = ctx;
            _ = sz;
            out.* = std.mem.zeroes(c_abi.JofAuditResult);
            out.info = .{
                .width = 4,
                .height = 2,
                .tile_w = 4,
                .tile_h = 1,
                .tile_cols = 1,
                .tile_rows = 2,
                .bpp = 1,
                .codec = c_abi.k_jof_codec_raw,
                .tile_count = if (mode == .too_many_tiles) 4098 else 2,
                .index_off = 36,
                .total_size = 68,
            };
            if (ws.records != null and ws.record_cap >= 2) {
                ws.records.?[0] = .{
                    .offset = 32,
                    .length = 2,
                    .payload = 2,
                    .content_hash = 0x1234ABCD,
                    .width = 4,
                    .height = 1,
                    .uniform = false,
                };
                ws.records.?[1] = .{
                    .offset = 34,
                    .length = 2,
                    .payload = 2,
                    .content_hash = 0xCAFE0001,
                    .width = 4,
                    .height = 1,
                    .uniform = true,
                };
            }
            if (mode == .dups) out.duplicate_candidates = 3;
            if (mode == .invalid) return c_abi.k_ra8_err_validation_failed;
            return c_abi.k_ra8_ok;
        }
    };
    d.jof_audit = H.audit;
    return d;
}

test "jofInspectStream formats concise valid report" {
    const bytes: [68]u8 = @splat(0);
    var src_state = mocks.MemSource{ .data = &bytes };
    const src = src_state.asSource(false);
    var records: [4]c_abi.JofAuditRecord = undefined;
    var tile: [16]u8 = undefined;
    var scratch: [16]u8 = undefined;
    const ws = c_abi.JofInspectWorkspace{
        .records = &records,
        .record_cap = 4,
        .tile = &tile,
        .tile_cap = 16,
        .scratch = &scratch,
        .scratch_cap = 16,
    };
    var sink_state = mocks.MemSink{};
    const sink = sink_state.asSink();

    const rc = inspect.jofInspectStream(comptime makeMockDeps(.ok), &src, false, &ws, &sink);
    try std.testing.expectEqual(c_abi.k_ra8_ok, rc);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "JOF atlas: 68 bytes") != null);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "longstrip  : YES (tile_w == width)") != null);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "verdict: VALID (coverage exact, no duplicate tiles)") != null);
}

test "jofInspectStream verbose includes hex dumps and uppercase hash table" {
    var bytes: [68]u8 = undefined;
    for (&bytes, 0..) |*b, i| b.* = @intCast(i & 0x7F);
    @memcpy(bytes[0..4], "JOF1");
    @memcpy(bytes[52..56], "JOFE");

    var src_state = mocks.MemSource{ .data = &bytes };
    const src = src_state.asSource(false);
    var records: [2]c_abi.JofAuditRecord = undefined;
    var tile: [16]u8 = undefined;
    var scratch: [16]u8 = undefined;
    const ws = c_abi.JofInspectWorkspace{
        .records = &records,
        .record_cap = 2,
        .tile = &tile,
        .tile_cap = 16,
        .scratch = &scratch,
        .scratch_cap = 16,
    };
    var sink_state = mocks.MemSink{};
    const sink = sink_state.asSink();

    const rc = inspect.jofInspectStream(comptime makeMockDeps(.dups), &src, true, &ws, &sink);
    try std.testing.expectEqual(c_abi.k_ra8_ok, rc);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "header (32 bytes at +0):") != null);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "footer (16 bytes at +52):") != null);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "1234ABCD") != null);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "duplicate hash candidates: 3 (diagnostic only)") != null);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "duplicate hashes are diagnostic") != null);
}

test "jofInspectStream preserves invalid verdict and audit failure status" {
    const bytes: [68]u8 = @splat(0);
    var src_state = mocks.MemSource{ .data = &bytes };
    const src = src_state.asSource(false);
    var records: [2]c_abi.JofAuditRecord = undefined;
    const ws = c_abi.JofInspectWorkspace{
        .records = &records,
        .record_cap = 2,
        .tile = null,
        .tile_cap = 0,
        .scratch = null,
        .scratch_cap = 0,
    };
    var sink_state = mocks.MemSink{};
    const sink = sink_state.asSink();

    const rc = inspect.jofInspectStream(comptime makeMockDeps(.invalid), &src, false, &ws, &sink);
    try std.testing.expectEqual(c_abi.k_ra8_err_validation_failed, rc);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "verdict: INVALID (see anomalies above)") != null);
}

test "jofInspectStream rejects null args and sink overflow" {
    const bytes: [68]u8 = @splat(0);
    var src_state = mocks.MemSource{ .data = &bytes };
    const src = src_state.asSource(false);
    var records: [2]c_abi.JofAuditRecord = undefined;
    const ws = c_abi.JofInspectWorkspace{
        .records = &records,
        .record_cap = 2,
        .tile = null,
        .tile_cap = 0,
        .scratch = null,
        .scratch_cap = 0,
    };
    var sink_state = mocks.MemSink{ .fail_after = 12, .fail_rc = c_abi.k_ra8_err_no_mem };
    const sink = sink_state.asSink();

    try std.testing.expectEqual(
        c_abi.k_ra8_err_null_ptr,
        inspect.jofInspectStream(comptime makeMockDeps(.ok), null, false, &ws, &sink),
    );
    try std.testing.expectEqual(
        c_abi.k_ra8_err_no_mem,
        inspect.jofInspectStream(comptime makeMockDeps(.ok), &src, false, &ws, &sink),
    );
}
