//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The media RPC seam, scripted, for the Zig test binaries.
//!
//! `src/internal/mdl_transfer.zig` calls three C functions that live in
//! `src/ra8_c6link_mdl.c`. A pure-Zig test binary links no C, so this file
//! stands in for them and lets a test say exactly what the co-processor
//! answers. In the installed archive the real C definitions are the ones that
//! link; nothing here reaches production.
//!
//! `types` is whichever module the test binary already carries the wire types
//! in, so a binary never ends up with two copies of `mdl_types.zig`.

const types = @import("types").mdl_types;

/// One scripted answer to a `next` pull.
pub const Answer = struct {
    err: u16 = 0,
    state: u8 = types.State.downloading,
    data_len: u16 = 0,
    total_bytes: u64 = 0,
    has_sha256: bool = false,
    sha256: [32]u8 = @splat(0),
    fill: u8 = 0,
};

/// What the scripted RPC does and what it was asked to do.
pub const Script = struct {
    answers: []const Answer = &.{},
    start_err: u16 = 0,
    cancel_err: u16 = 0,
    pulls: usize = 0,
    cancels: usize = 0,
    starts: usize = 0,
    last_max_bytes: u16 = 0,
};

pub var script: Script = .{};

/// Reset the seam and load one run's answers.
pub fn load(answers: []const Answer) void {
    script = .{ .answers = answers };
}

pub export fn ra8_c6link_mdl_start_request(
    link: ?*anyopaque,
    request: *const types.Request,
    session: *types.Session,
) callconv(.c) u16 {
    _ = link;
    script.starts += 1;
    if (script.start_err != 0) return script.start_err;
    session.* = .{ .job_id = 1, .format = request.format, .active = true };
    return 0;
}

pub export fn ra8_c6link_mdl_next(
    link: ?*anyopaque,
    session: *types.Session,
    max_bytes: u16,
    chunk: *types.Chunk,
) callconv(.c) u16 {
    _ = link;
    script.last_max_bytes = max_bytes;
    if (script.pulls >= script.answers.len) return types.Format.invalid;
    const answer = script.answers[script.pulls];
    script.pulls += 1;
    if (answer.err != 0) return answer.err;

    chunk.* = .{
        .job_id = session.job_id,
        .state = answer.state,
        .total_bytes = answer.total_bytes,
        .data_len = answer.data_len,
        .has_sha256 = answer.has_sha256,
        .sha256 = answer.sha256,
    };
    @memset(chunk.data[0..answer.data_len], answer.fill);
    if (answer.state != types.State.downloading) session.active = false;
    return 0;
}

pub export fn ra8_c6link_mdl_cancel(link: ?*anyopaque, session: *types.Session) callconv(.c) u16 {
    _ = link;
    script.cancels += 1;
    session.active = false;
    return script.cancel_err;
}
