//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the ethos-u adapter logic (RA8FW-573).

const std = @import("std");
const shim = @import("ethosu_shim");

const stream = [_]u32{ 0x0000_0001, 0x0000_0000 };
const bases = [_]u64{ 0x2200_0000, 0x2201_0000, 0x6A00_0000 };
const drv = shim.Driver{};

fn ok() shim.Invoke {
    return .{ .drv = &drv, .cmd = &stream, .size = 8, .bases = &bases, .count = 3 };
}

test "Job matches the ra8_npu_job_t C layout" {
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(shim.Job, "region_base"));
    try std.testing.expectEqual(@as(usize, 16 + 8 * 8), @sizeOf(shim.Job));
    try std.testing.expectEqual(@as(usize, 1), @sizeOf(shim.Driver));
}

test "valid arguments pass and build the job" {
    const a = ok();
    try shim.check(a);
    const job = shim.buildJob(a);
    try std.testing.expectEqual(@as(?*const anyopaque, &stream), job.cmd_stream);
    try std.testing.expectEqual(@as(u32, 8), job.cmd_stream_bytes);
    try std.testing.expectEqual(@as(u8, 3), job.region_count);
    try std.testing.expectEqualSlices(u64, &bases, job.region_base[0..3]);
    try std.testing.expectEqual(@as(u64, 0), job.region_base[3]);
}

test "zero regions need no base array" {
    var a = ok();
    a.count = 0;
    a.bases = null;
    try shim.check(a);
    try std.testing.expectEqual(@as(u8, 0), shim.buildJob(a).region_count);
}

test "rejections follow the C order" {
    var a = ok();
    a.drv = null;
    a.cmd = null;
    try std.testing.expectError(error.NullDriver, shim.check(a));
    a = ok();
    a.cmd = null;
    a.size = 0;
    try std.testing.expectError(error.NullStream, shim.check(a));
    a = ok();
    a.size = 0;
    try std.testing.expectError(error.EmptyStream, shim.check(a));
    a = ok();
    a.size = -4;
    try std.testing.expectError(error.EmptyStream, shim.check(a));
}

test "region count is bounded before any copy" {
    var a = ok();
    a.count = -1;
    try std.testing.expectError(error.NegativeCount, shim.check(a));
    a.count = 9;
    a.bases = null;
    try std.testing.expectError(error.TooManyRegions, shim.check(a));
    a.count = 8;
    try std.testing.expectError(error.NullBases, shim.check(a));
}

test "every rejection has the C log line" {
    try std.testing.expectEqualStrings("invoke: null driver", std.mem.span(shim.message(error.NullDriver)));
    try std.testing.expectEqualStrings("invoke: too many regions", std.mem.span(shim.message(error.TooManyRegions)));
    try std.testing.expectEqualStrings("invoke: null base_addr array", std.mem.span(shim.message(error.NullBases)));
}
