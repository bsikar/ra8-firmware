//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the bounded media transfer coordinator, driven against
//! a scripted RPC seam and recording storage and hash seams.

const std = @import("std");

const implementation = @import("implementation");
const mdl = implementation.mdl_transfer;
const types = implementation.mdl_types;
const stub = @import("mdl_rpc_stub");

/// A storage and hash backend that records the order it was driven in.
const Recorder = struct {
    var bytes: [4096]u8 = @splat(0);
    var length: usize = 0;
    var hashed: usize = 0;
    var begins: usize = 0;
    var commits: usize = 0;
    var aborts: usize = 0;
    var validates: usize = 0;
    var digest: [32]u8 = @splat(0);
    var write_err: u16 = 0;
    var short_write: bool = false;
    var commit_err: u16 = 0;
    var abort_err: u16 = 0;
    var validate_err: u16 = 0;
    var begin_err: u16 = 0;
    var final_err: u16 = 0;
    var cancel_asked: bool = false;

    fn reset() void {
        length = 0;
        hashed = 0;
        begins = 0;
        commits = 0;
        aborts = 0;
        validates = 0;
        digest = @splat(0);
        write_err = 0;
        short_write = false;
        commit_err = 0;
        abort_err = 0;
        validate_err = 0;
        begin_err = 0;
        final_err = 0;
        cancel_asked = false;
    }

    fn begin(ctx: ?*anyopaque, destination: ?[*:0]const u8) callconv(.c) u16 {
        _ = ctx;
        _ = destination;
        begins += 1;
        if (begin_err != 0) return begin_err;
        length = 0;
        return 0;
    }

    fn write(ctx: ?*anyopaque, data: ?[*]const u8, len: u16, written: ?*u16) callconv(.c) u16 {
        _ = ctx;
        written.?.* = 0;
        if (write_err != 0) return write_err;
        @memcpy(bytes[length..][0..len], data.?[0..len]);
        length += len;
        written.?.* = if (short_write) len - 1 else len;
        return 0;
    }

    fn validate(ctx: ?*anyopaque, total: u64, sha: ?[*]const u8) callconv(.c) u16 {
        _ = ctx;
        _ = total;
        _ = sha;
        validates += 1;
        return validate_err;
    }

    fn commit(ctx: ?*anyopaque) callconv(.c) u16 {
        _ = ctx;
        if (commit_err != 0) return commit_err;
        commits += 1;
        return 0;
    }

    fn abort(ctx: ?*anyopaque) callconv(.c) u16 {
        _ = ctx;
        aborts += 1;
        return abort_err;
    }

    fn hashInit(ctx: ?*anyopaque) callconv(.c) u16 {
        _ = ctx;
        hashed = 0;
        return 0;
    }

    fn hashUpdate(ctx: ?*anyopaque, data: ?[*]const u8, len: u16) callconv(.c) u16 {
        _ = ctx;
        _ = data;
        hashed += len;
        return 0;
    }

    fn hashFinal(ctx: ?*anyopaque, out: ?[*]u8) callconv(.c) u16 {
        _ = ctx;
        if (final_err != 0) return final_err;
        @memcpy(out.?[0..32], &digest);
        return 0;
    }

    fn cancelRequested(ctx: ?*anyopaque) callconv(.c) bool {
        _ = ctx;
        return cancel_asked;
    }
};

var fixture_ctx: u8 = 0;

fn config() types.Config {
    return .{
        .storage = .{
            .begin = Recorder.begin,
            .write = Recorder.write,
            .validate = Recorder.validate,
            .commit = Recorder.commit,
            .abort = Recorder.abort,
            .ctx = &fixture_ctx,
        },
        .sha256 = .{
            .init = Recorder.hashInit,
            .update = Recorder.hashUpdate,
            .final = Recorder.hashFinal,
            .ctx = &fixture_ctx,
        },
        .format = types.Format.loose,
        .chunk_bytes = 256,
        .max_chunks = 8,
    };
}

var link_handle: u8 = 0;

fn run(cfg: *const types.Config, result: *types.Result) u16 {
    return mdl.transfer(&link_handle, "https://example/x", "dest", cfg, result);
}

test "a two-chunk transfer hashes, commits, and reports the digest" {
    Recorder.reset();
    Recorder.digest = @splat(0xAB);
    stub.load(&.{
        .{ .state = types.State.downloading, .data_len = 10, .fill = 0x11 },
        .{ .state = types.State.complete, .total_bytes = 10, .has_sha256 = true, .sha256 = @splat(0xAB) },
    });
    const cfg = config();
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0), run(&cfg, &result));
    try std.testing.expectEqual(@as(u64, 10), result.bytes_stored);
    try std.testing.expectEqual(@as(u32, 2), result.chunks_received);
    try std.testing.expectEqualSlices(u8, &[_]u8{0xAB} ** 32, &result.sha256);
    try std.testing.expectEqual(@as(usize, 10), Recorder.length);
    try std.testing.expectEqual(@as(usize, 10), Recorder.hashed);
    try std.testing.expectEqual(@as(usize, 1), Recorder.commits);
    try std.testing.expectEqual(@as(usize, 0), Recorder.aborts);
}

test "the pull size the caller configured is what is asked for" {
    Recorder.reset();
    stub.load(&.{.{ .state = types.State.complete, .total_bytes = 0, .has_sha256 = true }});
    var cfg = config();
    cfg.chunk_bytes = 64;
    var result = types.Result{};
    _ = run(&cfg, &result);
    try std.testing.expectEqual(@as(u16, 64), stub.script.last_max_bytes);
}

test "a digest disagreement refuses the object and unwinds" {
    Recorder.reset();
    Recorder.digest = @splat(0x01);
    stub.load(&.{
        .{ .state = types.State.complete, .total_bytes = 0, .has_sha256 = true, .sha256 = @splat(0x02) },
    });
    const cfg = config();
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x502), run(&cfg, &result));
    try std.testing.expectEqual(@as(usize, 0), Recorder.commits);
    try std.testing.expectEqual(@as(usize, 1), Recorder.aborts);
}

test "a byte-count disagreement is refused before the digest is finalised" {
    Recorder.reset();
    stub.load(&.{
        .{ .state = types.State.downloading, .data_len = 4 },
        .{ .state = types.State.complete, .total_bytes = 99, .has_sha256 = true },
    });
    const cfg = config();
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x105), run(&cfg, &result));
    try std.testing.expectEqual(@as(usize, 1), Recorder.aborts);
}

test "a terminal response without a digest is refused" {
    Recorder.reset();
    stub.load(&.{.{ .state = types.State.complete, .total_bytes = 0, .has_sha256 = false }});
    const cfg = config();
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x105), run(&cfg, &result));
}

test "a short write is refused even though storage reported success" {
    Recorder.reset();
    Recorder.short_write = true;
    stub.load(&.{.{ .state = types.State.downloading, .data_len = 8 }});
    const cfg = config();
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x105), run(&cfg, &result));
    try std.testing.expectEqual(@as(usize, 0), Recorder.hashed);
}

test "a storage write failure is relayed verbatim" {
    Recorder.reset();
    Recorder.write_err = 0x777;
    stub.load(&.{.{ .state = types.State.downloading, .data_len = 8 }});
    const cfg = config();
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x777), run(&cfg, &result));
}

test "a cooperative cancel stops before the first pull" {
    Recorder.reset();
    Recorder.cancel_asked = true;
    stub.load(&.{.{ .state = types.State.complete, .total_bytes = 0, .has_sha256 = true }});
    var cfg = config();
    cfg.cancel_requested = Recorder.cancelRequested;
    cfg.cancel_ctx = &fixture_ctx;
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x10E), run(&cfg, &result));
    try std.testing.expectEqual(@as(usize, 0), stub.script.pulls);
    try std.testing.expectEqual(@as(usize, 1), stub.script.cancels);
    try std.testing.expectEqual(@as(usize, 1), Recorder.aborts);
}

test "a remote cancellation becomes the cancelled status" {
    Recorder.reset();
    stub.load(&.{.{ .state = types.State.cancelled }});
    const cfg = config();
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x10E), run(&cfg, &result));
}

test "exhausting the chunk budget with a live session is a timeout" {
    Recorder.reset();
    stub.load(&.{
        .{ .state = types.State.downloading, .data_len = 1 },
        .{ .state = types.State.downloading, .data_len = 1 },
    });
    var cfg = config();
    cfg.max_chunks = 2;
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x108), run(&cfg, &result));
    try std.testing.expectEqual(@as(usize, 2), stub.script.pulls);
    try std.testing.expectEqual(@as(usize, 1), stub.script.cancels);
}

test "a failed start leaves storage open for the caller to unwind" {
    Recorder.reset();
    stub.load(&.{});
    stub.script.start_err = 0x321;
    const cfg = config();
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x321), run(&cfg, &result));
    try std.testing.expectEqual(@as(usize, 1), Recorder.begins);
    try std.testing.expectEqual(@as(usize, 1), Recorder.aborts);
}

test "a storage begin failure touches neither the remote nor cleanup" {
    Recorder.reset();
    Recorder.begin_err = 0x654;
    stub.load(&.{});
    const cfg = config();
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x654), run(&cfg, &result));
    try std.testing.expectEqual(@as(usize, 0), stub.script.starts);
    try std.testing.expectEqual(@as(usize, 0), Recorder.aborts);
}

test "an original cause outranks a cleanup failure" {
    Recorder.reset();
    Recorder.abort_err = 0x999;
    stub.load(&.{.{ .state = types.State.cancelled }});
    const cfg = config();
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x10E), run(&cfg, &result));
}

test "a cleanup failure surfaces when there is no earlier cause" {
    Recorder.reset();
    Recorder.abort_err = 0x999;
    var state = mdl.State{ .config = undefined, .storage_active = true };
    const cfg = config();
    state.config = &cfg;
    try std.testing.expectEqual(@as(u16, 0x999), mdl.unwind(&state, 0));
    try std.testing.expect(!state.storage_active);
}

test "a non-loose format must bring an artifact validator" {
    Recorder.reset();
    stub.load(&.{});
    var cfg = config();
    cfg.format = types.Format.rabook;
    cfg.storage.validate = null;
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x504), run(&cfg, &result));
    try std.testing.expectEqual(@as(usize, 0), Recorder.begins);
}

test "a non-loose format runs its validator before publication" {
    Recorder.reset();
    stub.load(&.{.{ .state = types.State.complete, .total_bytes = 0, .has_sha256 = true }});
    var cfg = config();
    cfg.format = types.Format.rabook;
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0), run(&cfg, &result));
    try std.testing.expectEqual(@as(usize, 1), Recorder.validates);
    try std.testing.expectEqual(types.Format.rabook, result.format);
}

test "a validator refusal keeps the object private" {
    Recorder.reset();
    Recorder.validate_err = 0x111;
    stub.load(&.{.{ .state = types.State.complete, .total_bytes = 0, .has_sha256 = true }});
    var cfg = config();
    cfg.format = types.Format.rabook;
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x111), run(&cfg, &result));
    try std.testing.expectEqual(@as(usize, 0), Recorder.commits);
}

test "an unknown format is refused as an invalid argument" {
    Recorder.reset();
    var cfg = config();
    cfg.format = types.Format.invalid;
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0x103), run(&cfg, &result));
}

test "a missing storage or hash entry point is refused" {
    Recorder.reset();
    var result = types.Result{};
    var without_write = config();
    without_write.storage.write = null;
    try std.testing.expectEqual(@as(u16, 0x504), run(&without_write, &result));
    var without_final = config();
    without_final.sha256.final = null;
    try std.testing.expectEqual(@as(u16, 0x504), run(&without_final, &result));
    var without_ctx = config();
    without_ctx.storage.ctx = null;
    try std.testing.expectEqual(@as(u16, 0x504), run(&without_ctx, &result));
}

test "the chunk and budget bounds are enforced" {
    Recorder.reset();
    var result = types.Result{};
    var zero_chunk = config();
    zero_chunk.chunk_bytes = 0;
    try std.testing.expectEqual(@as(u16, 0x105), run(&zero_chunk, &result));
    var over_chunk = config();
    over_chunk.chunk_bytes = types.Limit.chunk_data_max + 1;
    try std.testing.expectEqual(@as(u16, 0x105), run(&over_chunk, &result));
    var zero_chunks = config();
    zero_chunks.max_chunks = 0;
    try std.testing.expectEqual(@as(u16, 0x105), run(&zero_chunks, &result));
    var over_budget = config();
    over_budget.chunk_bytes = 1024;
    over_budget.max_chunks = 65537;
    try std.testing.expectEqual(@as(u16, 0x105), run(&over_budget, &result));
}

test "the whole budget is spendable right up to the ceiling" {
    Recorder.reset();
    stub.load(&.{.{ .state = types.State.complete, .total_bytes = 0, .has_sha256 = true }});
    var cfg = config();
    cfg.chunk_bytes = 1024;
    cfg.max_chunks = 65536;
    var result = types.Result{};
    try std.testing.expectEqual(@as(u16, 0), run(&cfg, &result));
}
