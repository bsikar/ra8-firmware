//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the ci-monitor status reader's pure core
//! (#858, #1144). Every case here is a rule the Python held and monitor.sh
//! depends on, including the two rules that were each paid for with a wrong
//! verdict in anger (#530 skipped is not success, #561 cancelled is not
//! failure).

const std = @import("std");
const implementation = @import("implementation");

const Value = implementation.Value;

fn parse(text: []const u8) !std.json.Parsed(Value) {
    return std.json.parseFromSlice(Value, std.testing.allocator, text, .{});
}

fn expectVerdict(document: []const u8, sha: []const u8, want: []const u8) !void {
    var parsed = try parse(document);
    defer parsed.deinit();
    const runs = try implementation.runsOf(parsed.value);
    const answer = try implementation.verdict(std.testing.allocator, runs, sha);
    defer std.testing.allocator.free(answer);
    try std.testing.expectEqualStrings(want, answer);
}

fn expectField(document: []const u8, key: []const u8, want: []const u8) !void {
    var parsed = try parse(document);
    defer parsed.deinit();
    const text = try implementation.fieldText(std.testing.allocator, parsed.value, key);
    defer std.testing.allocator.free(text);
    try std.testing.expectEqualStrings(want, text);
}

fn expectRender(run_json: []const u8, with_sha: bool, want: []const u8) !void {
    var parsed = try parse(run_json);
    defer parsed.deinit();
    const line = try implementation.renderRun(std.testing.allocator, parsed.value, with_sha);
    defer std.testing.allocator.free(line);
    try std.testing.expectEqualStrings(want, line);
}

fn expectFloat(number: f64, want: []const u8) !void {
    const text = try implementation.pythonFloat(std.testing.allocator, number);
    defer std.testing.allocator.free(text);
    try std.testing.expectEqualStrings(want, text);
}

// ---------------------------------------------------------------- constants

test "the decisive conclusions are the two Python listed" {
    try std.testing.expectEqual(@as(usize, 2), implementation.fail_conclusions.len);
    try std.testing.expectEqualStrings("failure", implementation.fail_conclusions[0]);
    try std.testing.expectEqualStrings("timed_out", implementation.fail_conclusions[1]);
}

test "the head view and sha abbreviation keep their widths" {
    try std.testing.expectEqual(@as(usize, 6), implementation.head_rows);
    try std.testing.expectEqual(@as(usize, 9), implementation.sha_abbrev);
}

// --------------------------------------------------------------- truthiness

test "null, false, zero and empty values are falsy as Python read them" {
    var parsed = try parse("[null,false,0,0.0,\"\",[],{}]");
    defer parsed.deinit();
    for (parsed.value.array.items) |item| {
        try std.testing.expect(!implementation.isTruthy(item));
    }
}

test "non-empty values are truthy" {
    var parsed = try parse("[true,1,-1,0.5,\"x\",[0],{\"k\":0}]");
    defer parsed.deinit();
    for (parsed.value.array.items) |item| {
        try std.testing.expect(implementation.isTruthy(item));
    }
}

// ------------------------------------------------------------ value renders

test "a float renders in Python's fixed form with a fractional digit" {
    try expectFloat(1.0, "1.0");
    try expectFloat(0.5, "0.5");
    try expectFloat(-2.25, "-2.25");
    try expectFloat(0.0, "0.0");
    try expectFloat(123456789.0, "123456789.0");
}

test "a float switches to a signed two-digit exponent outside -4..15" {
    try expectFloat(1e30, "1e+30");
    try expectFloat(1e16, "1e+16");
    try expectFloat(1e-5, "1e-05");
    try expectFloat(1.5e30, "1.5e+30");
}

test "1e15 stays fixed, the last exponent Python spells out" {
    try expectFloat(1e15, "1000000000000000.0");
    try expectFloat(1e-4, "0.0001");
}

test "str() of a scalar matches Python's spelling" {
    var parsed = try parse("[null,true,false,5,\"text\"]");
    defer parsed.deinit();
    const wants = [_][]const u8{ "None", "True", "False", "5", "text" };
    for (parsed.value.array.items, wants) |item, want| {
        const text = try implementation.pythonStr(std.testing.allocator, item);
        defer std.testing.allocator.free(text);
        try std.testing.expectEqualStrings(want, text);
    }
}

// ------------------------------------------------------------- field access

test "a field prints its string value" {
    try expectField("{\"overall\":\"PASS\"}", "overall", "PASS");
}

test "a falsy field prints the empty string, not its value" {
    try expectField("{\"reason\":\"\"}", "reason", "");
    try expectField("{\"n\":0}", "n", "");
    try expectField("{\"n\":false}", "n", "");
    try expectField("{\"n\":null}", "n", "");
}

test "a missing field prints the empty string" {
    try expectField("{\"overall\":\"PASS\"}", "warning", "");
}

test "an empty key is a lookup like any other" {
    try expectField("{\"\":\"edge\"}", "", "edge");
    try expectField("{\"overall\":\"PASS\"}", "", "");
}

test "a truthy non-string field renders through str()" {
    try expectField("{\"n\":7}", "n", "7");
    try expectField("{\"n\":true}", "n", "True");
}

// ------------------------------------------------------------- document shape

test "a missing runs list is an empty list, not an error" {
    var parsed = try parse("{\"overall\":\"PASS\"}");
    defer parsed.deinit();
    const runs = try implementation.runsOf(parsed.value);
    try std.testing.expectEqual(@as(usize, 0), runs.len);
}

test "a falsy runs value is an empty list, matching `or []`" {
    var parsed = try parse("{\"runs\":null}");
    defer parsed.deinit();
    try std.testing.expectEqual(@as(usize, 0), (try implementation.runsOf(parsed.value)).len);
    var empty_object = try parse("{\"runs\":{}}");
    defer empty_object.deinit();
    try std.testing.expectEqual(@as(usize, 0), (try implementation.runsOf(empty_object.value)).len);
}

test "a truthy non-list runs value is the shape Python raised on" {
    var parsed = try parse("{\"runs\":{\"a\":1}}");
    defer parsed.deinit();
    try std.testing.expectError(error.RunsNotAList, implementation.runsOf(parsed.value));
}

test "a non-object document is the shape Python raised on" {
    var parsed = try parse("[1,2]");
    defer parsed.deinit();
    try std.testing.expectError(error.DocumentNotAnObject, implementation.runsOf(parsed.value));
}

test "a non-object run entry is rejected, never skipped" {
    var parsed = try parse("{\"runs\":[{\"name\":\"a\"},7]}");
    defer parsed.deinit();
    const runs = try implementation.runsOf(parsed.value);
    try std.testing.expectError(error.RunNotAnObject, implementation.requireRunObjects(runs));
}

// ------------------------------------------------------------ sha matching

test "a sha matches by prefix" {
    var parsed = try parse("{\"sha\":\"abc123\"}");
    defer parsed.deinit();
    try std.testing.expect(try implementation.matchesSha(std.testing.allocator, parsed.value, "abc"));
    try std.testing.expect(!try implementation.matchesSha(std.testing.allocator, parsed.value, "abd"));
}

test "the empty prefix matches every run, including one with no sha" {
    var parsed = try parse("{\"name\":\"a\"}");
    defer parsed.deinit();
    try std.testing.expect(try implementation.matchesSha(std.testing.allocator, parsed.value, ""));
    try std.testing.expect(!try implementation.matchesSha(std.testing.allocator, parsed.value, "a"));
}

test "a null sha reads as the empty string, not the word None" {
    var parsed = try parse("{\"sha\":null}");
    defer parsed.deinit();
    const text = try implementation.runText(std.testing.allocator, parsed.value, "sha");
    defer std.testing.allocator.free(text);
    try std.testing.expectEqualStrings("", text);
}

test "matching keeps document order" {
    var parsed = try parse(
        \\{"runs":[{"sha":"aa1"},{"sha":"bb2"},{"sha":"aa3"}]}
    );
    defer parsed.deinit();
    const runs = try implementation.runsOf(parsed.value);
    const got = try implementation.matching(std.testing.allocator, runs, "aa");
    defer std.testing.allocator.free(got);
    try std.testing.expectEqual(@as(usize, 2), got.len);
    try std.testing.expectEqualStrings("aa1", got[0].object.get("sha").?.string);
    try std.testing.expectEqualStrings("aa3", got[1].object.get("sha").?.string);
}

// ------------------------------------------------------------- conclusions

test "a conclusion compares as a string only" {
    var parsed = try parse("{\"conclusion\":\"success\"}");
    defer parsed.deinit();
    try std.testing.expect(implementation.conclusionIs(parsed.value, "success"));
    try std.testing.expect(!implementation.conclusionIs(parsed.value, "failure"));
    var numeric = try parse("{\"conclusion\":1}");
    defer numeric.deinit();
    try std.testing.expect(!implementation.conclusionIs(numeric.value, "1"));
}

test "failure and timed_out are decisive, cancelled and skipped are not" {
    const cases = [_]struct { json: []const u8, decisive: bool }{
        .{ .json = "{\"conclusion\":\"success\"}", .decisive = true },
        .{ .json = "{\"conclusion\":\"failure\"}", .decisive = true },
        .{ .json = "{\"conclusion\":\"timed_out\"}", .decisive = true },
        .{ .json = "{\"conclusion\":\"cancelled\"}", .decisive = false },
        .{ .json = "{\"conclusion\":\"skipped\"}", .decisive = false },
        .{ .json = "{\"conclusion\":null}", .decisive = false },
        .{ .json = "{}", .decisive = false },
    };
    for (cases) |case| {
        var parsed = try parse(case.json);
        defer parsed.deinit();
        try std.testing.expectEqual(case.decisive, implementation.isDecisive(parsed.value));
    }
}

test "a non-string or missing status counts as still in flight" {
    var completed = try parse("{\"status\":\"completed\"}");
    defer completed.deinit();
    try std.testing.expect(!implementation.isInFlight(completed.value));
    var running = try parse("{\"status\":\"in_progress\"}");
    defer running.deinit();
    try std.testing.expect(implementation.isInFlight(running.value));
    var missing = try parse("{}");
    defer missing.deinit();
    try std.testing.expect(implementation.isInFlight(missing.value));
    var numeric = try parse("{\"status\":1}");
    defer numeric.deinit();
    try std.testing.expect(implementation.isInFlight(numeric.value));
}

test "a conclusion count counts only that sha's runs" {
    var parsed = try parse(
        \\{"runs":[{"sha":"aa1","conclusion":"skipped"},{"sha":"bb2","conclusion":"skipped"},
        \\{"sha":"aa2","conclusion":"cancelled"}]}
    );
    defer parsed.deinit();
    const runs = try implementation.runsOf(parsed.value);
    try std.testing.expectEqual(
        @as(usize, 1),
        try implementation.conclusionCount(std.testing.allocator, runs, "aa", "skipped"),
    );
    try std.testing.expectEqual(
        @as(usize, 1),
        try implementation.conclusionCount(std.testing.allocator, runs, "aa", "cancelled"),
    );
    try std.testing.expectEqual(
        @as(usize, 2),
        try implementation.conclusionCount(std.testing.allocator, runs, "", "skipped"),
    );
}

// ----------------------------------------------------------------- renders

test "a run renders as an indented name, status and conclusion" {
    try expectRender(
        "{\"name\":\"firmware\",\"status\":\"completed\",\"conclusion\":\"success\"}",
        false,
        "  firmware: completed/success",
    );
}

test "a falsy conclusion renders as a dash" {
    try expectRender(
        "{\"name\":\"docs\",\"status\":\"in_progress\",\"conclusion\":null}",
        false,
        "  docs: in_progress/-",
    );
    try expectRender("{\"name\":\"docs\",\"status\":\"queued\"}", false, "  docs: queued/-");
}

test "a missing name or status renders as None, not as an empty field" {
    try expectRender("{}", false, "  None: None/-");
}

test "the head row appends the abbreviated sha after two spaces" {
    try expectRender(
        "{\"name\":\"firmware\",\"status\":\"completed\",\"conclusion\":\"success\",\"sha\":\"0123456789abcdef\"}",
        true,
        "  firmware: completed/success  012345678",
    );
}

test "a head row with no sha still carries the two trailing spaces" {
    try expectRender("{\"name\":\"a\",\"status\":\"completed\",\"conclusion\":null}", true, "  a: completed/-  ");
}

test "a sha abbreviation cuts on code points, never mid-sequence" {
    try std.testing.expectEqualStrings(
        "\u{00e9}\u{00e9}",
        implementation.truncateCodePoints("\u{00e9}\u{00e9}\u{00e9}", 2),
    );
    try std.testing.expectEqualStrings("abc", implementation.truncateCodePoints("abc", 9));
    try std.testing.expectEqualStrings("", implementation.truncateCodePoints("abc", 0));
}

test "a numeric name renders through str() in a row" {
    try expectRender("{\"name\":7,\"status\":\"completed\",\"conclusion\":\"failure\"}", false, "  7: completed/failure");
}

// -------------------------------------------------------- workflow verdicts

test "one successful run is a passing workflow" {
    var parsed = try parse("[{\"status\":\"completed\",\"conclusion\":\"success\"}]");
    defer parsed.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.pass,
        try implementation.workflowVerdict(std.testing.allocator, parsed.value.array.items),
    );
}

test "a timed-out run fails its workflow like a failure does" {
    var parsed = try parse("[{\"status\":\"completed\",\"conclusion\":\"timed_out\"}]");
    defer parsed.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.fail,
        try implementation.workflowVerdict(std.testing.allocator, parsed.value.array.items),
    );
}

test "a cancelled-only workflow is a non-result, not a failure (#561)" {
    var parsed = try parse("[{\"status\":\"completed\",\"conclusion\":\"cancelled\"}]");
    defer parsed.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.noresult,
        try implementation.workflowVerdict(std.testing.allocator, parsed.value.array.items),
    );
}

test "a skipped-only workflow is a non-result, never a pass (#530)" {
    var parsed = try parse("[{\"status\":\"completed\",\"conclusion\":\"skipped\"}]");
    defer parsed.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.noresult,
        try implementation.workflowVerdict(std.testing.allocator, parsed.value.array.items),
    );
}

test "an in-flight run keeps its workflow running" {
    var parsed = try parse("[{\"status\":\"in_progress\",\"conclusion\":null}]");
    defer parsed.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.running,
        try implementation.workflowVerdict(std.testing.allocator, parsed.value.array.items),
    );
}

test "a decisive run outranks an in-flight sibling" {
    var parsed = try parse(
        \\[{"status":"in_progress","conclusion":null},{"status":"completed","conclusion":"failure"}]
    );
    defer parsed.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.fail,
        try implementation.workflowVerdict(std.testing.allocator, parsed.value.array.items),
    );
}

test "the latest decisive run by created wins in either direction" {
    var cleared = try parse(
        \\[{"status":"completed","conclusion":"failure","created":"2026-08-01T00:00:00Z"},
        \\{"status":"completed","conclusion":"success","created":"2026-08-02T00:00:00Z"}]
    );
    defer cleared.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.pass,
        try implementation.workflowVerdict(std.testing.allocator, cleared.value.array.items),
    );
    var broken = try parse(
        \\[{"status":"completed","conclusion":"success","created":"2026-08-01T00:00:00Z"},
        \\{"status":"completed","conclusion":"failure","created":"2026-08-02T00:00:00Z"}]
    );
    defer broken.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.fail,
        try implementation.workflowVerdict(std.testing.allocator, broken.value.array.items),
    );
}

test "runs with no created keep document order, so the last one decides" {
    var parsed = try parse(
        \\[{"status":"completed","conclusion":"failure"},{"status":"completed","conclusion":"success"}]
    );
    defer parsed.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.pass,
        try implementation.workflowVerdict(std.testing.allocator, parsed.value.array.items),
    );
}

test "a created ordering sorts as a string, not as a date" {
    var parsed = try parse(
        \\[{"status":"completed","conclusion":"success","created":"2026-08-10"},
        \\{"status":"completed","conclusion":"failure","created":"2026-08-9"}]
    );
    defer parsed.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.fail,
        try implementation.workflowVerdict(std.testing.allocator, parsed.value.array.items),
    );
}

test "an empty workflow is a non-result" {
    var parsed = try parse("[]");
    defer parsed.deinit();
    try std.testing.expectEqual(
        implementation.WorkflowVerdict.noresult,
        try implementation.workflowVerdict(std.testing.allocator, parsed.value.array.items),
    );
}

// ------------------------------------------------------------ grouping keys

test "a string name never collides with a numeric one" {
    var text = try parse("{\"name\":\"7\"}");
    defer text.deinit();
    var number = try parse("{\"name\":7}");
    defer number.deinit();
    const left = try implementation.nameKey(std.testing.allocator, text.value);
    defer std.testing.allocator.free(left);
    const right = try implementation.nameKey(std.testing.allocator, number.value);
    defer std.testing.allocator.free(right);
    try std.testing.expect(!std.mem.eql(u8, left, right));
}

test "numeric equality collapses as Python's dict keys did" {
    var integer = try parse("{\"name\":1}");
    defer integer.deinit();
    var float = try parse("{\"name\":1.0}");
    defer float.deinit();
    var boolean = try parse("{\"name\":true}");
    defer boolean.deinit();
    const a = try implementation.nameKey(std.testing.allocator, integer.value);
    defer std.testing.allocator.free(a);
    const b = try implementation.nameKey(std.testing.allocator, float.value);
    defer std.testing.allocator.free(b);
    const c = try implementation.nameKey(std.testing.allocator, boolean.value);
    defer std.testing.allocator.free(c);
    try std.testing.expectEqualStrings(a, b);
    try std.testing.expectEqualStrings(a, c);
}

test "a missing name groups with an explicit null name" {
    var missing = try parse("{}");
    defer missing.deinit();
    var null_name = try parse("{\"name\":null}");
    defer null_name.deinit();
    const a = try implementation.nameKey(std.testing.allocator, missing.value);
    defer std.testing.allocator.free(a);
    const b = try implementation.nameKey(std.testing.allocator, null_name.value);
    defer std.testing.allocator.free(b);
    try std.testing.expectEqualStrings(a, b);
}

test "a list or dict name is the unhashable key Python raised on" {
    var list = try parse("{\"name\":[1]}");
    defer list.deinit();
    try std.testing.expectError(
        error.NameNotHashable,
        implementation.nameKey(std.testing.allocator, list.value),
    );
    var dict = try parse("{\"name\":{\"a\":1}}");
    defer dict.deinit();
    try std.testing.expectError(
        error.NameNotHashable,
        implementation.nameKey(std.testing.allocator, dict.value),
    );
}

// --------------------------------------------------------- sha verdicts

test "no run for the sha is UNKNOWN, never a vacuous pass" {
    try expectVerdict("{\"runs\":[{\"sha\":\"aa1\",\"conclusion\":\"success\",\"status\":\"completed\"}]}", "deadbeef", "UNKNOWN");
}

test "an all-skipped sha is UNKNOWN, not PASS (#530)" {
    try expectVerdict(
        \\{"runs":[{"name":"firmware","status":"completed","conclusion":"skipped","sha":"dd4"},
        \\{"name":"docs","status":"completed","conclusion":"skipped","sha":"dd4"}]}
    , "dd4", "UNKNOWN");
}

test "a partially skipped sha still passes" {
    try expectVerdict(
        \\{"runs":[{"name":"firmware","status":"completed","conclusion":"success","sha":"ee5"},
        \\{"name":"hil","status":"completed","conclusion":"skipped","sha":"ee5"}]}
    , "ee5", "PASS");
}

test "an all-cancelled sha is UNKNOWN, not FAIL (#561)" {
    try expectVerdict(
        \\{"runs":[{"name":"firmware","status":"completed","conclusion":"cancelled","sha":"gg2"},
        \\{"name":"docs","status":"completed","conclusion":"cancelled","sha":"gg2"}]}
    , "gg2", "UNKNOWN");
}

test "success workflows beside cancelled-only workflows pass (the 91eef75dd case)" {
    try expectVerdict(
        \\{"runs":[{"name":"firmware","status":"completed","conclusion":"success","sha":"ff1"},
        \\{"name":"coverage","status":"completed","conclusion":"success","sha":"ff1"},
        \\{"name":"emulator-smoke","status":"completed","conclusion":"cancelled","sha":"ff1"},
        \\{"name":"hil","status":"completed","conclusion":"cancelled","sha":"ff1"}]}
    , "ff1", "PASS");
}

test "a real failure still fails next to a cancelled sibling" {
    try expectVerdict(
        \\{"runs":[{"name":"firmware","status":"completed","conclusion":"failure","sha":"hh3"},
        \\{"name":"docs","status":"completed","conclusion":"cancelled","sha":"hh3"}]}
    , "hh3", "FAIL");
}

test "a still-running sha is UNKNOWN, not PASS" {
    try expectVerdict(
        \\{"runs":[{"name":"firmware","status":"completed","conclusion":"success","sha":"cc3"},
        \\{"name":"docs","status":"in_progress","conclusion":null,"sha":"cc3"}]}
    , "cc3", "UNKNOWN");
}

test "a failure outranks an in-flight workflow" {
    try expectVerdict(
        \\{"runs":[{"name":"firmware","status":"completed","conclusion":"failure","sha":"xx1"},
        \\{"name":"docs","status":"in_progress","conclusion":null,"sha":"xx1"}]}
    , "xx1", "FAIL");
}

test "a success is not overridden by a cancelled sibling in the SAME workflow" {
    try expectVerdict(
        \\{"runs":[{"name":"firmware","status":"completed","conclusion":"success","sha":"kk6"},
        \\{"name":"firmware","status":"completed","conclusion":"cancelled","sha":"kk6"}]}
    , "kk6", "PASS");
}

test "the empty prefix judges every run in the document" {
    try expectVerdict(
        \\{"runs":[{"name":"firmware","status":"completed","conclusion":"success","sha":"aa1"},
        \\{"name":"docs","status":"completed","conclusion":"failure","sha":"bb2"}]}
    , "", "FAIL");
}
