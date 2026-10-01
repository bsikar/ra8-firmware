//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Service-side pull rules: correlation, coherence, and advance.

const std = @import("std");
const testing = std.testing;
const pull = @import("implementation").mdl_pull;

fn liveJob() pull.JobView {
    return .{ .next_offset = 4096, .active_job_id = 7, .active = true };
}

fn goodNext() pull.NextRequestView {
    return .{
        .acknowledged_offset = 4096,
        .protocol_version = pull.Bound.protocol_version,
        .job_id = 7,
        .max_bytes = 512,
    };
}

fn dataPull() pull.PullView {
    return .{
        .next_offset = 4096,
        .total = 100_000,
        .next_sequence = 9,
        .max_data = 512,
        .got = 512,
        .complete = false,
        .response_valid = false,
        .response_status = 0,
    };
}

fn terminalPull() pull.PullView {
    return .{
        .next_offset = 100_000,
        .total = 100_000,
        .next_sequence = 9,
        .max_data = 512,
        .got = 0,
        .complete = true,
        .response_valid = true,
        .response_status = 200,
    };
}

test "a correlated next request is accepted" {
    const job = liveJob();
    try testing.expect(pull.nextCorrelates(&goodNext(), &job));
}

test "next is refused on the wrong protocol version" {
    const job = liveJob();
    var req = goodNext();
    req.protocol_version = pull.Bound.protocol_version + 1;
    try testing.expect(!pull.nextCorrelates(&req, &job));
}

test "next is refused when no job is active" {
    var job = liveJob();
    job.active = false;
    try testing.expect(!pull.nextCorrelates(&goodNext(), &job));
}

test "next is refused for another job id" {
    const job = liveJob();
    var req = goodNext();
    req.job_id = 8;
    try testing.expect(!pull.nextCorrelates(&req, &job));
}

test "next is refused when the peer acknowledges a different offset" {
    const job = liveJob();
    var req = goodNext();
    req.acknowledged_offset = 4095;
    try testing.expect(!pull.nextCorrelates(&req, &job));
}

test "next is refused for a zero or oversized requested bound" {
    const job = liveJob();
    var req = goodNext();
    req.max_bytes = 0;
    try testing.expect(!pull.nextCorrelates(&req, &job));
    req.max_bytes = pull.Bound.chunk_data_max + 1;
    try testing.expect(!pull.nextCorrelates(&req, &job));
}

test "the maximum requested bound is still accepted" {
    const job = liveJob();
    var req = goodNext();
    req.max_bytes = pull.Bound.chunk_data_max;
    try testing.expect(pull.nextCorrelates(&req, &job));
}

test "cancel correlates on version and job id only" {
    const job = liveJob();
    const req: pull.CancelRequestView = .{
        .protocol_version = pull.Bound.protocol_version,
        .job_id = 7,
    };
    try testing.expect(pull.cancelCorrelates(&req, &job));
}

test "cancel is refused for an idle service or a stale job id" {
    var job = liveJob();
    const req: pull.CancelRequestView = .{
        .protocol_version = pull.Bound.protocol_version,
        .job_id = 7,
    };
    job.active = false;
    try testing.expect(!pull.cancelCorrelates(&req, &job));
    job.active = true;
    job.active_job_id = 8;
    try testing.expect(!pull.cancelCorrelates(&req, &job));
}

test "end offset adds the body and reports a wrap" {
    try testing.expectEqual(@as(?u64, 4608), pull.endOffset(4096, 512));
    try testing.expectEqual(@as(?u64, null), pull.endOffset(std.math.maxInt(u64), 1));
    try testing.expectEqual(
        @as(?u64, std.math.maxInt(u64)),
        pull.endOffset(std.math.maxInt(u64), 0),
    );
}

test "a terminal pull must close the declared total exactly" {
    try testing.expect(pull.totalCoherent(100, 100, true));
    try testing.expect(!pull.totalCoherent(99, 100, true));
    try testing.expect(!pull.totalCoherent(101, 100, true));
}

test "an unknown total accepts any non terminal position" {
    try testing.expect(pull.totalCoherent(1 << 40, 0, false));
}

test "a non terminal pull may not run past a declared total" {
    try testing.expect(pull.totalCoherent(100, 100, false));
    try testing.expect(!pull.totalCoherent(101, 100, false));
}

test "an ordinary data pull is coherent" {
    try testing.expect(pull.pullCoherent(&dataPull()));
}

test "a terminal pull is coherent" {
    try testing.expect(pull.pullCoherent(&terminalPull()));
}

test "a pull over the requested bound is refused" {
    var view = dataPull();
    view.got = 513;
    try testing.expect(!pull.pullCoherent(&view));
}

test "an empty non terminal pull is refused" {
    var view = dataPull();
    view.got = 0;
    try testing.expect(!pull.pullCoherent(&view));
}

test "a terminal pull carrying body bytes is refused" {
    var view = terminalPull();
    view.got = 1;
    try testing.expect(!pull.pullCoherent(&view));
}

test "a pull that overflows the offset space is refused" {
    var view = dataPull();
    view.next_offset = std.math.maxInt(u64);
    view.total = 0;
    try testing.expect(!pull.pullCoherent(&view));
}

test "a pull is refused once the sequence has no room left" {
    var view = dataPull();
    view.next_sequence = pull.Bound.sequence_max;
    try testing.expect(!pull.pullCoherent(&view));
    view.next_sequence = pull.Bound.sequence_max - 1;
    try testing.expect(pull.pullCoherent(&view));
}

test "a data pull running past the declared total is refused" {
    var view = dataPull();
    view.total = 4100;
    try testing.expect(!pull.pullCoherent(&view));
}

test "a terminal pull with malformed metadata is refused" {
    var view = terminalPull();
    view.response_valid = false;
    try testing.expect(!pull.pullCoherent(&view));
}

test "a data pull carrying an http status is refused" {
    var view = dataPull();
    view.response_status = 200;
    try testing.expect(!pull.pullCoherent(&view));
}

test "advance moves offset and sequence and keeps the job alive" {
    const next = pull.advance(4096, 9, 512, false);
    try testing.expectEqual(@as(u64, 4608), next.next_offset);
    try testing.expectEqual(@as(u32, 10), next.next_sequence);
    try testing.expect(next.active);
}

test "a terminal pull ends the job" {
    const next = pull.advance(100_000, 9, 0, true);
    try testing.expectEqual(@as(u64, 100_000), next.next_offset);
    try testing.expectEqual(@as(u32, 10), next.next_sequence);
    try testing.expect(!next.active);
}
