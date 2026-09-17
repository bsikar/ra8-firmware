//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression suite for the argv membrane, the report decoding
//! and the exit-status contract of the per-file MC/DC FLOOR gate. Every
//! file-system case runs against a temporary tree, so the statuses the
//! launcher passes through are exercised rather than described.

const std = @import("std");
const testing = std.testing;
const cli = @import("cli");

const repo = "ra8-firmware";

const Harness = struct {
    tmp: testing.TmpDir,
    buffer: std.ArrayList(u8),

    fn init() Harness {
        return .{
            .tmp = testing.tmpDir(.{}),
            .buffer = std.ArrayList(u8).init(testing.allocator),
        };
    }

    fn deinit(self: *Harness) void {
        self.buffer.deinit();
        self.tmp.cleanup();
    }

    fn writeReport(self: *Harness, data: []const u8) !void {
        try self.tmp.dir.makePath("root/build/mcdc-report");
        try self.tmp.dir.writeFile(.{
            .sub_path = "root/build/mcdc-report/mcdc_per_file.json",
            .data = data,
        });
    }

    fn run(self: *Harness, argv: []const []const u8) !u8 {
        return cli.run(
            testing.allocator,
            self.tmp.dir,
            "root",
            repo,
            argv,
            self.buffer.writer(),
        );
    }

    fn text(self: *Harness) []const u8 {
        return self.buffer.items;
    }
};

fn entry(path: []const u8, covered: i64, total: i64) cli.Entry {
    return .{ .file = path, .covered_decisions = covered, .total_decisions = total };
}

/// A report that satisfies every non-vacuity check, so a test can vary one
/// row without tripping the missing-scope branch first.
const full_report =
    \\{"files": [
    \\  {"file": "libs/ra8_core/src/core.c", "covered_decisions": 2, "total_decisions": 2},
    \\  {"file": "apps/shared_libs/book/src/book.c", "covered_decisions": 1, "total_decisions": 1},
    \\  {"file": "examples/ek_ra8d2/demo/src/main.c", "covered_decisions": 1, "total_decisions": 1},
    \\  {"file": "port/posix/src/io.c", "covered_decisions": 1, "total_decisions": 1},
    \\  {"file": "tools/ra8_emulator/src/main.c", "covered_decisions": 1, "total_decisions": 1}
    \\]}
;

// ------------------------------------------------------------ argv membrane

test "the selftest runs only when argv is exactly --selftest" {
    try testing.expect(cli.wantsSelftest(&[_][]const u8{"--selftest"}));
}

test "no arguments run the gate" {
    try testing.expect(!cli.wantsSelftest(&[_][]const u8{}));
}

test "an abbreviation is not the selftest, because the predecessor compared for equality" {
    try testing.expect(!cli.wantsSelftest(&[_][]const u8{"--self"}));
    try testing.expect(!cli.wantsSelftest(&[_][]const u8{"--s"}));
}

test "a repeated flag is not the selftest" {
    try testing.expect(!cli.wantsSelftest(&[_][]const u8{ "--selftest", "--selftest" }));
}

test "the flag with a trailing argument is not the selftest" {
    try testing.expect(!cli.wantsSelftest(&[_][]const u8{ "--selftest", "extra" }));
}

test "an unknown flag is not the selftest and is not a usage error either" {
    try testing.expect(!cli.wantsSelftest(&[_][]const u8{"--nope"}));
    try testing.expect(!cli.wantsSelftest(&[_][]const u8{"-h"}));
}

test "the flag is case sensitive" {
    try testing.expect(!cli.wantsSelftest(&[_][]const u8{"--SELFTEST"}));
}

test "an equals form is not the selftest" {
    try testing.expect(!cli.wantsSelftest(&[_][]const u8{"--selftest=1"}));
}

// ------------------------------------------------------------ exit statuses

test "there are exactly two statuses and no usage status" {
    try testing.expectEqual(@as(u8, 0), cli.exit_ok);
    try testing.expectEqual(@as(u8, 1), cli.exit_fail);
}

test "the report path is the one the regenerator writes" {
    try testing.expectEqualStrings("build/mcdc-report/mcdc_per_file.json", cli.report_path);
}

// ---------------------------------------------------------------- selftest

test "the selftest passes and says so" {
    var harness = Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"--selftest"}));
    try testing.expectEqualStrings(
        "check_mcdc_floor selftest: PASS -- scope and non-vacuity checks hold.\n",
        harness.text(),
    );
}

test "the selftest needs no report on disk" {
    var harness = Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"--selftest"}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "ERROR") == null);
}

test "the selftest covers every scope case in both directions" {
    try testing.expectEqual(@as(usize, 14), cli.scope_cases.len);
    var positives: usize = 0;
    for (cli.scope_cases) |case| {
        if (case.expected) positives += 1;
    }
    try testing.expectEqual(@as(usize, 5), positives);
}

test "the covered fixtures name one file per required root" {
    try testing.expectEqual(@as(usize, 5), cli.covered_fixtures.len);
}

// ------------------------------------------------------- missing report

test "an absent report fails rather than passing vacuously" {
    var harness = Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "not found; run `bash scripts/report/mcdc_report.sh` first.") != null);
}

test "the absent-report line names the path it looked for" {
    var harness = Harness.init();
    defer harness.deinit();
    _ = try harness.run(&[_][]const u8{});
    try testing.expect(std.mem.indexOf(u8, harness.text(), "root/build/mcdc-report/mcdc_per_file.json") != null);
}

test "a directory where the report belongs is treated as absent" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.tmp.dir.makePath("root/build/mcdc-report/mcdc_per_file.json");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "not found") != null);
}

// --------------------------------------------------------- unreadable input

test "invalid JSON fails with the read diagnostic" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport("{\"files\": [");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "cannot read MC/DC JSON:") != null);
}

test "a top-level array fails with the read diagnostic rather than a traceback" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport("[]");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "top-level value is not an object") != null);
}

test "a non-object files row fails with a diagnostic" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport("{\"files\": [1, 2]}");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "a `files` row is not an object") != null);
}

test "a non-string file field fails with a diagnostic" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport("{\"files\": [{\"file\": 7}]}");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "non-string `file`") != null);
}

test "a null decision count fails with a diagnostic" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport("{\"files\": [{\"file\": \"libs/a.c\", \"total_decisions\": null}]}");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "not an integer") != null);
}

// ------------------------------------------------------------- empty report

test "a report with an empty files array fails" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport("{\"files\": []}");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expectEqualStrings("check_mcdc_floor: ERROR -- MC/DC JSON has no files.\n", harness.text());
}

test "a report with no files key fails the same way" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport("{}");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "has no files") != null);
}

test "a non-array files value is reported as no files" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport("{\"files\": 3}");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "has no files") != null);
}

// -------------------------------------------------------------- non-vacuity

test "a missing production root fails the non-vacuity check" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(
        \\{"files": [{"file": "libs/ra8_core/src/core.c", "covered_decisions": 1, "total_decisions": 1}]}
    );
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "no reachable decisions matched required scope(s): apps/shared_libs/, examples/, port/, tools/; check the JSON path / scope.") != null);
}

test "an exempt-only report fails every non-vacuity check" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(
        \\{"files": [{"file": "libs/third_party/soup.c", "covered_decisions": 0, "total_decisions": 4}]}
    );
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "libs/, apps/shared_libs/, examples/, port/, tools/") != null);
}

test "the non-vacuity check runs before the offender table" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(
        \\{"files": [{"file": "libs/ra8_core/src/core.c", "covered_decisions": 0, "total_decisions": 4}]}
    );
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "no reachable decisions matched") != null);
    try testing.expect(std.mem.indexOf(u8, harness.text(), "mc/dc  covered/reachable") == null);
}

// ------------------------------------------------------------------- verdict

test "a fully covered report passes and counts the files" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(full_report);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
    try testing.expectEqualStrings(
        "check_mcdc_floor: PASS -- all 5 first-party file(s) with a reachable decision are >= 100% MC/DC.\n",
        harness.text(),
    );
}

test "an unknown flag still runs the gate, as the predecessor did" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(full_report);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"--nope"}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "PASS --") != null);
}

test "--selftest with an extra argument runs the gate instead of the selftest" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(full_report);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{ "--selftest", "extra" }));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "selftest: PASS") == null);
}

test "one rotted file fails the whole report" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(
        \\{"files": [
        \\  {"file": "libs/ra8_core/src/core.c", "covered_decisions": 3, "total_decisions": 4},
        \\  {"file": "apps/shared_libs/book/src/book.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "examples/ek_ra8d2/demo/src/main.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "port/posix/src/io.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "tools/ra8_emulator/src/main.c", "covered_decisions": 1, "total_decisions": 1}
        \\]}
    );
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "1 first-party file(s) below the 100% reachable-MC/DC floor (NO allowlist):") != null);
    try testing.expect(std.mem.indexOf(u8, harness.text(), "   75.0%      3/4           libs/ra8_core/src/core.c\n") != null);
}

test "a deactivated-only gap passes, since the floor is the reachable rate" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(
        \\{"files": [
        \\  {"file": "libs/ra8_core/src/core.c", "covered_decisions": 2, "total_decisions": 5, "deactivated_decisions": 3},
        \\  {"file": "apps/shared_libs/book/src/book.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "examples/ek_ra8d2/demo/src/main.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "port/posix/src/io.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "tools/ra8_emulator/src/main.c", "covered_decisions": 1, "total_decisions": 1}
        \\]}
    );
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "all 5 first-party file(s)") != null);
}

test "a decision-free file is not counted in the passing census" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(
        \\{"files": [
        \\  {"file": "libs/ra8_core/src/core.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "libs/ra8_core/src/quiet.c", "covered_decisions": 0, "total_decisions": 0},
        \\  {"file": "apps/shared_libs/book/src/book.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "examples/ek_ra8d2/demo/src/main.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "port/posix/src/io.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "tools/ra8_emulator/src/main.c", "covered_decisions": 1, "total_decisions": 1}
        \\]}
    );
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "all 5 first-party file(s)") != null);
}

test "an absolute file field in the report is normalised before scoping" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(
        \\{"files": [
        \\  {"file": "/home/ci/ra8-firmware/libs/ra8_core/src/core.c", "covered_decisions": 1, "total_decisions": 2},
        \\  {"file": "apps/shared_libs/book/src/book.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "examples/ek_ra8d2/demo/src/main.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "port/posix/src/io.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "tools/ra8_emulator/src/main.c", "covered_decisions": 1, "total_decisions": 1}
        \\]}
    );
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "   50.0%      1/2           libs/ra8_core/src/core.c\n") != null);
}

test "several offenders print worst first" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(
        \\{"files": [
        \\  {"file": "libs/ra8_core/src/core.c", "covered_decisions": 3, "total_decisions": 4},
        \\  {"file": "apps/shared_libs/book/src/book.c", "covered_decisions": 0, "total_decisions": 2},
        \\  {"file": "examples/ek_ra8d2/demo/src/main.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "port/posix/src/io.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "tools/ra8_emulator/src/main.c", "covered_decisions": 1, "total_decisions": 1}
        \\]}
    );
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    const text = harness.text();
    const worst = std.mem.indexOf(u8, text, "apps/shared_libs/book/src/book.c").?;
    const next = std.mem.indexOf(u8, text, "libs/ra8_core/src/core.c").?;
    try testing.expect(worst < next);
    try testing.expect(std.mem.indexOf(u8, text, "2 first-party file(s) below") != null);
}

// --------------------------------------------------- decoding without files

test "a float decision count truncates toward zero, as int() did" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(
        \\{"files": [
        \\  {"file": "libs/ra8_core/src/core.c", "covered_decisions": 1.9, "total_decisions": 2.2},
        \\  {"file": "apps/shared_libs/book/src/book.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "examples/ek_ra8d2/demo/src/main.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "port/posix/src/io.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "tools/ra8_emulator/src/main.c", "covered_decisions": 1, "total_decisions": 1}
        \\]}
    );
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "   50.0%      1/2           libs/ra8_core/src/core.c\n") != null);
}

test "a decimal-string decision count parses" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.writeReport(
        \\{"files": [
        \\  {"file": "libs/ra8_core/src/core.c", "covered_decisions": "1", "total_decisions": "4"},
        \\  {"file": "apps/shared_libs/book/src/book.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "examples/ek_ra8d2/demo/src/main.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "port/posix/src/io.c", "covered_decisions": 1, "total_decisions": 1},
        \\  {"file": "tools/ra8_emulator/src/main.c", "covered_decisions": 1, "total_decisions": 1}
        \\]}
    );
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.text(), "   25.0%      1/4           libs/ra8_core/src/core.c\n") != null);
}

test "gateEntries judges a decoded report without touching a file system" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    const entries = [_]cli.Entry{
        entry("libs/a.c", 1, 1),
        entry("apps/shared_libs/b.c", 1, 1),
        entry("examples/c.c", 1, 1),
        entry("port/d.c", 1, 1),
        entry("tools/e.c", 1, 1),
    };
    const status = try cli.gateEntries(testing.allocator, &entries, repo, buffer.writer());
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expect(std.mem.indexOf(u8, buffer.items, "all 5 first-party file(s)") != null);
}

test "gateEntries reports the offender table for a rotted file" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    const entries = [_]cli.Entry{
        entry("libs/a.c", 1, 16),
        entry("apps/shared_libs/b.c", 1, 1),
        entry("examples/c.c", 1, 1),
        entry("port/d.c", 1, 1),
        entry("tools/e.c", 1, 1),
    };
    const status = try cli.gateEntries(testing.allocator, &entries, repo, buffer.writer());
    try testing.expectEqual(@as(u8, 1), status);
    // 1/16 is exactly 6.25, the round-half-to-even tie CPython prints as 6.2.
    try testing.expect(std.mem.indexOf(u8, buffer.items, "    6.2%      1/16          libs/a.c\n") != null);
}

test "entriesFromReport decodes the rows it is given" {
    var parsed = try std.json.parseFromSlice(std.json.Value, testing.allocator, full_report, .{});
    defer parsed.deinit();
    const entries = try cli.entriesFromReport(testing.allocator, parsed.value);
    defer testing.allocator.free(entries);
    try testing.expectEqual(@as(usize, 5), entries.len);
    try testing.expectEqualStrings("libs/ra8_core/src/core.c", entries[0].file);
    try testing.expectEqual(@as(i64, 2), entries[0].total_decisions);
    try testing.expectEqual(@as(i64, 0), entries[0].deactivated_decisions);
}

test "entriesFromReport refuses a non-object document" {
    var parsed = try std.json.parseFromSlice(std.json.Value, testing.allocator, "\"text\"", .{});
    defer parsed.deinit();
    try testing.expectError(error.NotAnObject, cli.entriesFromReport(testing.allocator, parsed.value));
}

test "entriesFromReport treats a missing files key as no rows" {
    var parsed = try std.json.parseFromSlice(std.json.Value, testing.allocator, "{}", .{});
    defer parsed.deinit();
    const entries = try cli.entriesFromReport(testing.allocator, parsed.value);
    defer testing.allocator.free(entries);
    try testing.expectEqual(@as(usize, 0), entries.len);
}

test "a boolean decision count decodes as one, the way int(True) did" {
    var parsed = try std.json.parseFromSlice(
        std.json.Value,
        testing.allocator,
        "{\"files\": [{\"file\": \"libs/a.c\", \"covered_decisions\": true, \"total_decisions\": true}]}",
        .{},
    );
    defer parsed.deinit();
    const entries = try cli.entriesFromReport(testing.allocator, parsed.value);
    defer testing.allocator.free(entries);
    try testing.expectEqual(@as(i64, 1), entries[0].covered_decisions);
    try testing.expectEqual(@as(i64, 1), entries[0].total_decisions);
}

test "a row with no counts decodes as zeroes and is skipped by the gate" {
    var parsed = try std.json.parseFromSlice(
        std.json.Value,
        testing.allocator,
        "{\"files\": [{\"file\": \"libs/a.c\"}]}",
        .{},
    );
    defer parsed.deinit();
    const entries = try cli.entriesFromReport(testing.allocator, parsed.value);
    defer testing.allocator.free(entries);
    try testing.expectEqual(@as(i64, 0), entries[0].total_decisions);
}

test "a row with no file field decodes to the empty path" {
    var parsed = try std.json.parseFromSlice(
        std.json.Value,
        testing.allocator,
        "{\"files\": [{\"total_decisions\": 2}]}",
        .{},
    );
    defer parsed.deinit();
    const entries = try cli.entriesFromReport(testing.allocator, parsed.value);
    defer testing.allocator.free(entries);
    try testing.expectEqualStrings("", entries[0].file);
}
