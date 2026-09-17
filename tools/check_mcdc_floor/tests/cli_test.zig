//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the argv membrane and the exit contract of
//! the per-file MC/DC floor gate (#858, #1205). Each case drives `cli.run`
//! against a temporary tree and pins the status plus the stream the predecessor
//! wrote on, because scripts/report/mcdc_report.sh branches on the status and
//! humans read the offender table out of the CI log.

const std = @import("std");
const cli = @import("cli");

const Outcome = struct {
    status: u8,
    stdout: []const u8,
    stderr: []const u8,

    fn deinit(self: *Outcome, allocator: std.mem.Allocator) void {
        allocator.free(self.stdout);
        allocator.free(self.stderr);
    }
};

/// Run the gate in a scratch tree holding `report` (null writes no report).
fn runWith(
    allocator: std.mem.Allocator,
    args: []const []const u8,
    report: ?[]const u8,
) !Outcome {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    if (report) |text| {
        try tmp.dir.makePath("build/mcdc-report");
        try tmp.dir.writeFile(.{ .sub_path = cli.mcdc_json_rel, .data = text });
    }

    var stdout = std.ArrayList(u8).init(allocator);
    errdefer stdout.deinit();
    var stderr = std.ArrayList(u8).init(allocator);
    errdefer stderr.deinit();

    const status = try cli.run(
        allocator,
        args,
        tmp.dir,
        "/w/ra8-firmware",
        "ra8-firmware",
        stdout.writer(),
        stderr.writer(),
    );

    return .{
        .status = status,
        .stdout = try stdout.toOwnedSlice(),
        .stderr = try stderr.toOwnedSlice(),
    };
}

/// A report whose five production roots each contribute one covered decision.
const clean_report =
    \\{"files":[
    \\ {"file":"libs/ra8_core/src/core.c","covered_decisions":3,"total_decisions":3},
    \\ {"file":"apps/shared_libs/book/src/book.c","covered_decisions":1,"total_decisions":1},
    \\ {"file":"examples/ek_ra8d2/demo/src/main.c","covered_decisions":2,"total_decisions":2},
    \\ {"file":"port/posix/src/io.c","covered_decisions":1,"total_decisions":1},
    \\ {"file":"tools/ra8_emulator/src/main.c","covered_decisions":4,"total_decisions":4}
    \\]}
;

fn reportWithOffender(comptime extra: []const u8) []const u8 {
    return "{\"files\":[" ++
        "{\"file\":\"libs/ra8_core/src/core.c\",\"covered_decisions\":3,\"total_decisions\":3}," ++
        "{\"file\":\"apps/shared_libs/book/src/book.c\",\"covered_decisions\":1,\"total_decisions\":1}," ++
        "{\"file\":\"examples/ek_ra8d2/demo/src/main.c\",\"covered_decisions\":2,\"total_decisions\":2}," ++
        "{\"file\":\"port/posix/src/io.c\",\"covered_decisions\":1,\"total_decisions\":1}," ++
        "{\"file\":\"tools/ra8_emulator/src/main.c\",\"covered_decisions\":4,\"total_decisions\":4}," ++
        extra ++
        "]}";
}

test "a clean report passes and names the checked count on stdout" {
    var outcome = try runWith(std.testing.allocator, &.{}, clean_report);
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), outcome.status);
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: PASS -- all 5 first-party file(s) with a reachable decision are >= 100% MC/DC.\n",
        outcome.stdout,
    );
    try std.testing.expectEqualStrings("", outcome.stderr);
}

test "one below-floor file fails with the offender table on stdout" {
    var outcome = try runWith(
        std.testing.allocator,
        &.{},
        reportWithOffender(
            "{\"file\":\"libs/ra8_spi/src/spi.c\",\"covered_decisions\":1,\"total_decisions\":4}",
        ),
    );
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: 1 first-party file(s) below the 100% reachable-MC/DC floor (NO allowlist):\n" ++
            "  mc/dc  covered/reachable  file\n" ++
            "   25.0%      1/4           libs/ra8_spi/src/spi.c\n" ++
            "Fix each at the root -- add the missing MC/DC vector (N+1 vectors for N conditions; " ++
            "see docs/MCDC.md), or, if the gap is genuinely unreachable on any public-API path, " ++
            "catalogue it with a `// mcdc-deactivated:` rationale per DO-178C 6.4.4.3. " ++
            "Do NOT add an allowlist.\n",
        outcome.stdout,
    );
    try std.testing.expectEqualStrings("", outcome.stderr);
}

test "offenders print worst first" {
    var outcome = try runWith(
        std.testing.allocator,
        &.{},
        reportWithOffender(
            "{\"file\":\"libs/b.c\",\"covered_decisions\":1,\"total_decisions\":2}," ++
                "{\"file\":\"libs/a.c\",\"covered_decisions\":0,\"total_decisions\":2}",
        ),
    );
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    const worst = std.mem.indexOf(u8, outcome.stdout, "libs/a.c").?;
    const second = std.mem.indexOf(u8, outcome.stdout, "libs/b.c").?;
    try std.testing.expect(worst < second);
}

test "a deactivated-only gap stays at full marks and passes" {
    var outcome = try runWith(
        std.testing.allocator,
        &.{},
        reportWithOffender(
            "{\"file\":\"libs/ra8_dma/src/dma.c\",\"covered_decisions\":2," ++
                "\"total_decisions\":6,\"deactivated_decisions\":4}",
        ),
    );
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), outcome.status);
    try std.testing.expect(std.mem.indexOf(u8, outcome.stdout, "all 6 first-party") != null);
}

test "a file with no reachable decision is skipped, not counted" {
    var outcome = try runWith(
        std.testing.allocator,
        &.{},
        reportWithOffender(
            "{\"file\":\"libs/ra8_nodec/src/plain.c\",\"covered_decisions\":0,\"total_decisions\":0}",
        ),
    );
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), outcome.status);
    try std.testing.expect(std.mem.indexOf(u8, outcome.stdout, "all 5 first-party") != null);
}

test "a report missing one production root fails its non-vacuity check" {
    var outcome = try runWith(std.testing.allocator, &.{},
        \\{"files":[
        \\ {"file":"libs/a.c","covered_decisions":1,"total_decisions":1},
        \\ {"file":"apps/shared_libs/b.c","covered_decisions":1,"total_decisions":1},
        \\ {"file":"examples/c.c","covered_decisions":1,"total_decisions":1},
        \\ {"file":"port/d.c","covered_decisions":1,"total_decisions":1}
        \\]}
    );
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- no reachable decisions matched required scope(s): " ++
            "tools/; check the JSON path / scope.\n",
        outcome.stdout,
    );
}

test "the non-vacuity check runs BEFORE the offender report" {
    var outcome = try runWith(std.testing.allocator, &.{},
        \\{"files":[{"file":"libs/a.c","covered_decisions":0,"total_decisions":4}]}
    );
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expect(std.mem.indexOf(u8, outcome.stdout, "required scope(s)") != null);
    try std.testing.expect(std.mem.indexOf(u8, outcome.stdout, "NO allowlist") == null);
}

test "a vendored-only report fails every production non-vacuity check" {
    var outcome = try runWith(std.testing.allocator, &.{},
        \\{"files":[{"file":"libs/third_party/soup.c","covered_decisions":0,"total_decisions":9}]}
    );
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- no reachable decisions matched required scope(s): " ++
            "libs/, apps/shared_libs/, examples/, port/, tools/; check the JSON path / scope.\n",
        outcome.stdout,
    );
}

test "an absent report is an error, never a vacuous pass" {
    var outcome = try runWith(std.testing.allocator, &.{}, null);
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- /w/ra8-firmware/build/mcdc-report/mcdc_per_file.json not found; " ++
            "run `bash scripts/report/mcdc_report.sh` first.\n",
        outcome.stdout,
    );
    try std.testing.expectEqualStrings("", outcome.stderr);
}

test "a directory where the report belongs takes the not-found branch" {
    const allocator = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath(cli.mcdc_json_rel);

    var stdout = std.ArrayList(u8).init(allocator);
    defer stdout.deinit();
    var stderr = std.ArrayList(u8).init(allocator);
    defer stderr.deinit();

    const status = try cli.run(
        allocator,
        &.{},
        tmp.dir,
        "/w/ra8-firmware",
        "ra8-firmware",
        stdout.writer(),
        stderr.writer(),
    );
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, stdout.items, "not found;") != null);
}

test "an unparseable report is reported as unreadable on stdout" {
    var outcome = try runWith(std.testing.allocator, &.{}, "{\"files\": [");
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expect(std.mem.startsWith(
        u8,
        outcome.stdout,
        "check_mcdc_floor: ERROR -- cannot read MC/DC JSON: ",
    ));
    try std.testing.expectEqualStrings("", outcome.stderr);
}

test "an empty files list is the no-files error" {
    var outcome = try runWith(std.testing.allocator, &.{}, "{\"files\": []}");
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- MC/DC JSON has no files.\n",
        outcome.stdout,
    );
}

test "a missing files key is the no-files error" {
    var outcome = try runWith(std.testing.allocator, &.{}, "{\"totals\": {\"pct\": 100}}");
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- MC/DC JSON has no files.\n",
        outcome.stdout,
    );
}

test "every falsy files value is the no-files error, not a crash" {
    const allocator = std.testing.allocator;
    const falsy = [_][]const u8{
        "{\"files\": null}",
        "{\"files\": {}}",
        "{\"files\": \"\"}",
        "{\"files\": 0}",
        "{\"files\": false}",
    };
    for (falsy) |text| {
        var outcome = try runWith(allocator, &.{}, text);
        defer outcome.deinit(allocator);
        try std.testing.expectEqual(@as(u8, 1), outcome.status);
        try std.testing.expectEqualStrings(
            "check_mcdc_floor: ERROR -- MC/DC JSON has no files.\n",
            outcome.stdout,
        );
    }
}

test "a truthy non-list files value fails on stderr" {
    var outcome = try runWith(std.testing.allocator, &.{}, "{\"files\": {\"libs/a.c\": 1}}");
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings("", outcome.stdout);
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- MC/DC JSON `files` is not a list.\n",
        outcome.stderr,
    );
}

test "a document that is not an object fails on stderr" {
    var outcome = try runWith(std.testing.allocator, &.{}, "[{\"file\":\"libs/a.c\"}]");
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings("", outcome.stdout);
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- MC/DC JSON is not an object.\n",
        outcome.stderr,
    );
}

test "a malformed count on an in-scope entry fails on stderr" {
    var outcome = try runWith(
        std.testing.allocator,
        &.{},
        "{\"files\":[{\"file\":\"libs/a.c\",\"total_decisions\":null}]}",
    );
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expectEqualStrings("", outcome.stdout);
    try std.testing.expect(std.mem.indexOf(u8, outcome.stderr, "cannot read (TypeError)") != null);
}

test "a non-object entry fails on stderr" {
    var outcome = try runWith(std.testing.allocator, &.{}, "{\"files\":[\"libs/a.c\"]}");
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expect(std.mem.indexOf(u8, outcome.stderr, "AttributeError") != null);
}

test "a malformed count on an EXEMPT entry never reaches the reader" {
    var outcome = try runWith(
        std.testing.allocator,
        &.{},
        reportWithOffender("{\"file\":\"libs/third_party/soup.c\",\"total_decisions\":null}"),
    );
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), outcome.status);
    try std.testing.expectEqualStrings("", outcome.stderr);
}

test "absolute paths in the report normalise against the checkout basename" {
    const allocator = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("build/mcdc-report");
    try tmp.dir.writeFile(.{
        .sub_path = cli.mcdc_json_rel,
        .data =
        \\{"files":[{"file":"/build/agent/ra8-lane5/libs/a.c","covered_decisions":0,"total_decisions":2}]}
        ,
    });

    var stdout = std.ArrayList(u8).init(allocator);
    defer stdout.deinit();
    var stderr = std.ArrayList(u8).init(allocator);
    defer stderr.deinit();
    const status = try cli.run(
        allocator,
        &.{},
        tmp.dir,
        "/build/agent/ra8-lane5",
        "ra8-lane5",
        stdout.writer(),
        stderr.writer(),
    );
    try std.testing.expectEqual(@as(u8, 1), status);
    // Scoped as libs/a.c, so it is the missing-roots error rather than an
    // out-of-scope no-files pass.
    try std.testing.expect(std.mem.indexOf(u8, stdout.items, "apps/shared_libs/") != null);
}

test "--selftest passes on its own" {
    var outcome = try runWith(std.testing.allocator, &.{"--selftest"}, null);
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), outcome.status);
    try std.testing.expectEqualStrings(
        "check_mcdc_floor selftest: PASS -- scope and non-vacuity checks hold.\n",
        outcome.stdout,
    );
    try std.testing.expectEqualStrings("", outcome.stderr);
}

test "--selftest never touches the report" {
    var outcome = try runWith(std.testing.allocator, &.{"--selftest"}, "not json at all");
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), outcome.status);
    try std.testing.expect(std.mem.indexOf(u8, outcome.stdout, "selftest: PASS") != null);
}

test "--selftest only wins when it is the whole argv" {
    const allocator = std.testing.allocator;
    const not_selftest = [_][]const []const u8{
        &.{ "--selftest", "extra" },
        &.{ "extra", "--selftest" },
        &.{"--selftests"},
        &.{"selftest"},
    };
    for (not_selftest) |args| {
        var outcome = try runWith(allocator, args, clean_report);
        defer outcome.deinit(allocator);
        try std.testing.expectEqual(@as(u8, 0), outcome.status);
        try std.testing.expect(std.mem.indexOf(u8, outcome.stdout, "PASS -- all 5") != null);
    }
}

test "an unknown flag runs the gate rather than erroring" {
    var outcome = try runWith(std.testing.allocator, &.{"--nonsense"}, clean_report);
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), outcome.status);
    try std.testing.expect(std.mem.indexOf(u8, outcome.stdout, "PASS -- all 5") != null);
}

test "a stray positional runs the gate and keeps the failing status" {
    var outcome = try runWith(
        std.testing.allocator,
        &.{"build/other.json"},
        reportWithOffender("{\"file\":\"libs/x.c\",\"covered_decisions\":0,\"total_decisions\":1}"),
    );
    defer outcome.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), outcome.status);
    try std.testing.expect(std.mem.indexOf(u8, outcome.stdout, "libs/x.c") != null);
}

test "the gate never invents an exit status beyond 0 and 1" {
    const allocator = std.testing.allocator;
    const documents = [_]?[]const u8{
        clean_report,
        "{\"files\": []}",
        "not json",
        null,
        "{\"files\":[{\"file\":\"libs/a.c\",\"covered_decisions\":0,\"total_decisions\":1}]}",
    };
    for (documents) |document| {
        var outcome = try runWith(allocator, &.{}, document);
        defer outcome.deinit(allocator);
        try std.testing.expect(outcome.status == 0 or outcome.status == 1);
    }
}

test "python truthiness decides the no-files branch" {
    try std.testing.expect(!cli.truthy(.null));
    try std.testing.expect(!cli.truthy(.{ .bool = false }));
    try std.testing.expect(!cli.truthy(.{ .integer = 0 }));
    try std.testing.expect(!cli.truthy(.{ .float = 0.0 }));
    try std.testing.expect(!cli.truthy(.{ .string = "" }));
    try std.testing.expect(cli.truthy(.{ .integer = 1 }));
    try std.testing.expect(cli.truthy(.{ .string = "x" }));
}

test "a report with a thousand clean files still passes" {
    const allocator = std.testing.allocator;
    var document = std.ArrayList(u8).init(allocator);
    defer document.deinit();
    try document.appendSlice("{\"files\":[");
    for (0..1000) |index| {
        if (index != 0) try document.append(',');
        const root = cli_roots[index % cli_roots.len];
        try document.writer().print(
            "{{\"file\":\"{s}file{d}.c\",\"covered_decisions\":2,\"total_decisions\":2}}",
            .{ root, index },
        );
    }
    try document.appendSlice("]}");

    var outcome = try runWith(allocator, &.{}, document.items);
    defer outcome.deinit(allocator);
    try std.testing.expectEqual(@as(u8, 0), outcome.status);
    try std.testing.expect(std.mem.indexOf(u8, outcome.stdout, "all 1000 first-party") != null);
}

const cli_roots = [_][]const u8{
    "libs/",
    "apps/shared_libs/",
    "examples/",
    "port/",
    "tools/",
};

test "the tool name and report path are re-exported for the launcher" {
    try std.testing.expectEqualStrings("check_mcdc_floor", cli.tool_name);
    try std.testing.expectEqualStrings("build/mcdc-report/mcdc_per_file.json", cli.mcdc_json_rel);
}
