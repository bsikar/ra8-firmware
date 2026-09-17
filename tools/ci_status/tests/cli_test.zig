//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status and read-mode regression tests for the ci-monitor status
//! reader's argv membrane (#858, #1144). `monitor.sh` reads these exact bytes
//! and branches on these exact statuses, so each mode's output and each
//! failure's status is pinned here rather than left to the caller to discover.

const std = @import("std");
const cli = @import("cli");

const fixture =
    \\{"overall":"PASS","reason":"","polled_at":"2026-09-17T00:00:00Z","runs":[
    \\{"name":"firmware","status":"completed","conclusion":"success","sha":"aaaaaaaaa1"},
    \\{"name":"firmware","status":"completed","conclusion":"failure","sha":"bbbbbbbbb2"},
    \\{"name":"docs","status":"in_progress","conclusion":null,"sha":"ccccccccc3"},
    \\{"name":"firmware","status":"completed","conclusion":"skipped","sha":"ddddddddd4"},
    \\{"name":"docs","status":"completed","conclusion":"skipped","sha":"ddddddddd4"},
    \\{"name":"firmware","status":"completed","conclusion":"success","sha":"eeeeeeeee5"},
    \\{"name":"hil","status":"completed","conclusion":"skipped","sha":"eeeeeeeee5"}]}
;

const Outcome = struct {
    status: u8,
    out: []const u8,
    err: []const u8,

    fn deinit(self: Outcome, allocator: std.mem.Allocator) void {
        allocator.free(self.out);
        allocator.free(self.err);
    }
};

fn runWith(
    dir: std.fs.Dir,
    argv: []const []const u8,
) !Outcome {
    const allocator = std.testing.allocator;
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    errdefer err.deinit();
    const status = try cli.run(allocator, dir, argv, out.writer(), err.writer());
    return .{
        .status = status,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

const Fixture = struct {
    tmp: std.testing.TmpDir,

    fn init(text: []const u8) !Fixture {
        var tmp = std.testing.tmpDir(.{});
        try tmp.dir.writeFile(.{ .sub_path = "status.json", .data = text });
        return .{ .tmp = tmp };
    }

    fn deinit(self: *Fixture) void {
        self.tmp.cleanup();
    }
};

fn expectMode(argv: []const []const u8, want_status: u8, want_out: []const u8) !void {
    var state = try Fixture.init(fixture);
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, argv);
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(want_status, outcome.status);
    try std.testing.expectEqualStrings(want_out, outcome.out);
}

// ------------------------------------------------------------------ modes

test "field prints a document field and exits 0" {
    try expectMode(&.{ "status.json", "field", "overall" }, 0, "PASS\n");
}

test "field prints an empty line for a falsy field" {
    try expectMode(&.{ "status.json", "field", "reason" }, 0, "\n");
}

test "field prints an empty line for a missing field" {
    try expectMode(&.{ "status.json", "field", "warning" }, 0, "\n");
}

test "field with no argument reads the empty key and still exits 0" {
    try expectMode(&.{ "status.json", "field" }, 0, "\n");
}

test "count prints how many runs carry the sha prefix" {
    try expectMode(&.{ "status.json", "count", "ddddddddd4" }, 0, "2\n");
}

test "count with no argument counts every run" {
    try expectMode(&.{ "status.json", "count" }, 0, "7\n");
}

test "count prints zero for an unrecorded sha" {
    try expectMode(&.{ "status.json", "count", "deadbeef9" }, 0, "0\n");
}

test "verdict prints PASS for a healthy sha" {
    try expectMode(&.{ "status.json", "verdict", "aaaaaaaaa1" }, 0, "PASS\n");
}

test "verdict prints FAIL for a failing sha" {
    try expectMode(&.{ "status.json", "verdict", "bbbbbbbbb2" }, 0, "FAIL\n");
}

test "verdict prints UNKNOWN for an all-skipped sha (#530)" {
    try expectMode(&.{ "status.json", "verdict", "ddddddddd4" }, 0, "UNKNOWN\n");
}

test "verdict prints UNKNOWN for an unrecorded sha" {
    try expectMode(&.{ "status.json", "verdict", "deadbeef9" }, 0, "UNKNOWN\n");
}

test "verdict prints UNKNOWN while a workflow is in flight" {
    try expectMode(&.{ "status.json", "verdict", "ccccccccc3" }, 0, "UNKNOWN\n");
}

test "verdict prints PASS for a partially skipped sha" {
    try expectMode(&.{ "status.json", "verdict", "eeeeeeeee5" }, 0, "PASS\n");
}

test "skipped-count counts the sha's skipped runs" {
    try expectMode(&.{ "status.json", "skipped-count", "ddddddddd4" }, 0, "2\n");
    try expectMode(&.{ "status.json", "skipped-count", "eeeeeeeee5" }, 0, "1\n");
}

test "cancelled-count counts the sha's cancelled runs" {
    try expectMode(&.{ "status.json", "cancelled-count", "ddddddddd4" }, 0, "0\n");
}

test "lines-sha prints one row per matching run, without a sha column" {
    try expectMode(
        &.{ "status.json", "lines-sha", "eeeeeeeee5" },
        0,
        "  firmware: completed/success\n  hil: completed/skipped\n",
    );
}

test "lines-sha prints nothing for an unrecorded sha and still exits 0" {
    try expectMode(&.{ "status.json", "lines-sha", "deadbeef9" }, 0, "");
}

test "lines-head prints the first six runs with their abbreviated shas" {
    try expectMode(
        &.{ "status.json", "lines-head" },
        0,
        "  firmware: completed/success  aaaaaaaaa\n" ++
            "  firmware: completed/failure  bbbbbbbbb\n" ++
            "  docs: in_progress/-  ccccccccc\n" ++
            "  firmware: completed/skipped  ddddddddd\n" ++
            "  docs: completed/skipped  ddddddddd\n" ++
            "  firmware: completed/success  eeeeeeeee\n",
    );
}

test "arguments past the third are ignored" {
    try expectMode(&.{ "status.json", "field", "overall", "extra", "more" }, 0, "PASS\n");
}

// --------------------------------------------------------------- failures

test "an unknown mode exits 1 with the Python's message on stderr" {
    var state = try Fixture.init(fixture);
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, &.{ "status.json", "bogus" });
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings("", outcome.out);
    try std.testing.expectEqualStrings("unknown mode: bogus\n", outcome.err);
}

test "fewer than two arguments exits 1 with a usage line" {
    var state = try Fixture.init(fixture);
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, &.{"status.json"});
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expect(std.mem.indexOf(u8, outcome.err, "usage") != null);
}

test "no arguments at all exits 1" {
    var state = try Fixture.init(fixture);
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, &.{});
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
}

test "a missing state file exits 1, never a vacuous pass" {
    var state = try Fixture.init(fixture);
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, &.{ "absent.json", "verdict", "aaaaaaaaa1" });
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings("", outcome.out);
    try std.testing.expect(std.mem.indexOf(u8, outcome.err, "cannot read") != null);
}

test "a malformed document exits 1" {
    var state = try Fixture.init("{\"runs\": [");
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, &.{ "status.json", "field", "overall" });
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expect(std.mem.indexOf(u8, outcome.err, "cannot parse") != null);
}

test "a non-object document exits 1" {
    var state = try Fixture.init("[1,2,3]");
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, &.{ "status.json", "field", "overall" });
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
}

test "a truthy non-list runs value exits 1" {
    var state = try Fixture.init("{\"runs\":{\"a\":1}}");
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, &.{ "status.json", "count", "a" });
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
}

test "a non-object run entry exits 1" {
    var state = try Fixture.init("{\"runs\":[7]}");
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, &.{ "status.json", "count", "a" });
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
}

test "an unhashable workflow name exits 1 rather than inventing a group" {
    var state = try Fixture.init("{\"runs\":[{\"name\":[1],\"sha\":\"aa1\",\"status\":\"completed\",\"conclusion\":\"success\"}]}");
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, &.{ "status.json", "verdict", "aa1" });
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
}

test "a document with no runs answers every run mode without failing" {
    var state = try Fixture.init("{\"overall\":\"UNKNOWN\"}");
    defer state.deinit();
    const counted = try runWith(state.tmp.dir, &.{ "status.json", "count", "aa" });
    defer counted.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), counted.status);
    try std.testing.expectEqualStrings("0\n", counted.out);

    const judged = try runWith(state.tmp.dir, &.{ "status.json", "verdict", "aa" });
    defer judged.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), judged.status);
    try std.testing.expectEqualStrings("UNKNOWN\n", judged.out);

    const head = try runWith(state.tmp.dir, &.{ "status.json", "lines-head" });
    defer head.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), head.status);
    try std.testing.expectEqualStrings("", head.out);
}

test "lines-head prints every run when the document holds fewer than six" {
    var state = try Fixture.init(
        \\{"runs":[{"name":"a","status":"completed","conclusion":"success","sha":"aa1"}]}
    );
    defer state.deinit();
    const outcome = try runWith(state.tmp.dir, &.{ "status.json", "lines-head" });
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), outcome.status);
    try std.testing.expectEqualStrings("  a: completed/success  aa1\n", outcome.out);
}

test "a state file past any plausible read ceiling is still answered" {
    // The Python's `json.load` had no size limit, so a document that grew
    // large stayed readable. A ceiling here would have turned it into
    // "cannot read" and status 1 instead, losing the verdict the monitor
    // branches on, so the whole file is read.
    const allocator = std.testing.allocator;
    const pad_bytes: usize = 17 * 1024 * 1024;
    var text = std.ArrayList(u8).init(allocator);
    defer text.deinit();
    try text.appendSlice("{\"overall\":\"PASS\",\"pad\":\"");
    try text.appendNTimes('x', pad_bytes);
    try text.appendSlice("\",\"runs\":[{\"name\":\"firmware\",\"status\":\"completed\"," ++
        "\"conclusion\":\"success\",\"sha\":\"aaaaaaaaa1\"}]}");

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "status.json", .data = text.items });

    const judged = try runWith(tmp.dir, &.{ "status.json", "verdict", "aaaaaaaaa1" });
    defer judged.deinit(allocator);
    try std.testing.expectEqual(@as(u8, 0), judged.status);
    try std.testing.expectEqualStrings("PASS\n", judged.out);

    const named = try runWith(tmp.dir, &.{ "status.json", "field", "overall" });
    defer named.deinit(allocator);
    try std.testing.expectEqual(@as(u8, 0), named.status);
    try std.testing.expectEqualStrings("PASS\n", named.out);
}
