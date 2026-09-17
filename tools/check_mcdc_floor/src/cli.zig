//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane, report decoding and exit-status contract for the per-file
//! MC/DC FLOOR gate (#858).
//!
//! `run` is parameterised on a directory, a repository root, the checkout
//! basename and one output stream, so the whole contract, file-system
//! statuses included, is exercised against a temporary tree rather than the
//! real repository.
//!
//! Exit statuses, inherited verbatim from the predecessor:
//!
//!   0  every in-scope file with a reachable decision is at the floor, or the
//!      scope selftest held in both directions
//!   1  an offender, a missing or unreadable report, a report with no files,
//!      a production root with no reachable decision, or a failing selftest
//!
//! There is deliberately NO usage status and no option parsing: the
//! predecessor ran its selftest when argv was EXACTLY `["--selftest"]` and
//! ran the gate for anything else, so `--self`, `--selftest --selftest` and
//! an unknown flag all run the gate. Both directions are pinned by tests.
//!
//! Everything the gate prints goes to stdout, as the predecessor's `print`
//! did; nothing downstream parses it, and the report is read out of the CI
//! log by a human.

const std = @import("std");
const implementation = @import("internal/root.zig");

pub const tool = implementation.tool;
pub const Entry = implementation.Entry;

pub const exit_ok: u8 = 0;
pub const exit_fail: u8 = 1;

/// Where the regenerator writes the per-file roll-up, relative to the root.
pub const report_path = "build/mcdc-report/mcdc_per_file.json";

const max_report_bytes = 64 * 1024 * 1024;

pub const DecodeError = error{
    NotAnObject,
    RowNotAnObject,
    FileNotAString,
    CountNotAnInteger,
};

/// CPython's `int(...)` over a JSON value, which is what `entry.get(...)`
/// was handed: a bool is 1/0, a float truncates toward zero, and a decimal
/// string parses. Anything else raised a `TypeError` in the predecessor and
/// reached its uncaught-exception exit 1; here it is one diagnostic and the
/// same status.
fn decisionCount(value: ?std.json.Value) DecodeError!i64 {
    const present = value orelse return 0;
    return switch (present) {
        .integer => |n| n,
        .bool => |b| @intFromBool(b),
        .float => |f| blk: {
            if (!std.math.isFinite(f)) return error.CountNotAnInteger;
            break :blk @intFromFloat(@trunc(f));
        },
        .number_string, .string => |text| std.fmt.parseInt(i64, std.mem.trim(u8, text, " \t\n\r"), 10) catch
            return error.CountNotAnInteger,
        else => error.CountNotAnInteger,
    };
}

fn entryFromRow(row: std.json.Value) DecodeError!Entry {
    const object = switch (row) {
        .object => |map| map,
        else => return error.RowNotAnObject,
    };
    const file = switch (object.get("file") orelse std.json.Value{ .string = "" }) {
        .string => |text| text,
        else => return error.FileNotAString,
    };
    return .{
        .file = file,
        .covered_decisions = try decisionCount(object.get("covered_decisions")),
        .total_decisions = try decisionCount(object.get("total_decisions")),
        .deactivated_decisions = try decisionCount(object.get("deactivated_decisions")),
    };
}

/// Decode the `files` array of a parsed report. A missing, empty or
/// non-array `files` comes back as an empty slice, which the caller reports
/// as "has no files" exactly as the predecessor's falsy test did.
pub fn entriesFromReport(
    allocator: std.mem.Allocator,
    document: std.json.Value,
) (DecodeError || std.mem.Allocator.Error)![]Entry {
    const object = switch (document) {
        .object => |map| map,
        else => return error.NotAnObject,
    };
    const files = switch (object.get("files") orelse std.json.Value{ .null = {} }) {
        .array => |rows| rows,
        else => return allocator.alloc(Entry, 0),
    };
    var entries = try allocator.alloc(Entry, files.items.len);
    errdefer allocator.free(entries);
    for (files.items, 0..) |row, index| entries[index] = try entryFromRow(row);
    return entries;
}

fn describeDecodeError(err: DecodeError) []const u8 {
    return switch (err) {
        error.NotAnObject => "top-level value is not an object",
        error.RowNotAnObject => "a `files` row is not an object",
        error.FileNotAString => "a `files` row has a non-string `file`",
        error.CountNotAnInteger => "a decision count is not an integer",
    };
}

/// Run the floor over one decoded report. Split out so the gate's whole
/// verdict is testable without a file system.
pub fn gateEntries(
    allocator: std.mem.Allocator,
    entries: []const Entry,
    repo_name: []const u8,
    out: anytype,
) !u8 {
    var collected = try implementation.collectOffenders(allocator, entries, repo_name);
    defer collected.deinit(allocator);

    const missing = try implementation.missingScopes(allocator, collected.counts);
    defer allocator.free(missing);
    if (missing.len != 0) {
        try out.print("{s}: ERROR -- no reachable decisions matched required scope(s): ", .{tool});
        for (missing, 0..) |prefix, index| {
            if (index != 0) try out.writeAll(", ");
            try out.writeAll(prefix);
        }
        try out.writeAll("; check the JSON path / scope.\n");
        return exit_fail;
    }

    if (collected.offenders.len != 0) {
        try implementation.writeOffenderReport(out, collected.offenders);
        return exit_fail;
    }

    try implementation.writePassLine(out, collected.counts.total());
    return exit_ok;
}

/// The gate: read the report the regenerator wrote, then judge it.
pub fn gate(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    repo_name: []const u8,
    out: anytype,
) !u8 {
    const path = try std.fs.path.join(allocator, &[_][]const u8{ repo_root, report_path });
    defer allocator.free(path);

    // `Path.is_file()` is false for a directory as well as for an absent
    // path, and both took the same branch in the predecessor.
    const stat = dir.statFile(path) catch {
        try out.print(
            "{s}: ERROR -- {s} not found; run `bash scripts/report/mcdc_report.sh` first.\n",
            .{ tool, path },
        );
        return exit_fail;
    };
    if (stat.kind == .directory) {
        try out.print(
            "{s}: ERROR -- {s} not found; run `bash scripts/report/mcdc_report.sh` first.\n",
            .{ tool, path },
        );
        return exit_fail;
    }

    const text = dir.readFileAlloc(allocator, path, max_report_bytes) catch |err| {
        try out.print("{s}: ERROR -- cannot read MC/DC JSON: {s}\n", .{ tool, @errorName(err) });
        return exit_fail;
    };
    defer allocator.free(text);

    var parsed = std.json.parseFromSlice(std.json.Value, allocator, text, .{}) catch |err| {
        try out.print("{s}: ERROR -- cannot read MC/DC JSON: {s}\n", .{ tool, @errorName(err) });
        return exit_fail;
    };
    defer parsed.deinit();

    const entries = entriesFromReport(allocator, parsed.value) catch |err| switch (err) {
        error.OutOfMemory => return error.OutOfMemory,
        else => {
            try out.print(
                "{s}: ERROR -- cannot read MC/DC JSON: {s}\n",
                .{ tool, describeDecodeError(@errorCast(err)) },
            );
            return exit_fail;
        },
    };
    defer allocator.free(entries);

    if (entries.len == 0) {
        try out.print("{s}: ERROR -- MC/DC JSON has no files.\n", .{tool});
        return exit_fail;
    }

    return gateEntries(allocator, entries, repo_name, out);
}

/// One scope fixture: a path and whether the floor should cover it.
pub const ScopeCase = struct {
    path: []const u8,
    expected: bool,
};

/// The predecessor's scope table, carried over case for case.
pub const scope_cases = [_]ScopeCase{
    .{ .path = "libs/ra8_core/src/core.c", .expected = true },
    .{ .path = "apps/shared_libs/book/src/book.c", .expected = true },
    .{ .path = "examples/ek_ra8d2/demo/src/main.c", .expected = true },
    .{ .path = "port/posix/src/io.c", .expected = true },
    .{ .path = "tools/ra8_emulator/src/main.c", .expected = true },
    .{ .path = "libs/third_party/soup.c", .expected = false },
    .{ .path = "apps/shared_libs/third_party/soup/source.c", .expected = false },
    .{ .path = "libs/ra8_fonts/src/generated.c", .expected = false },
    .{ .path = "apps/shared_libs/book/tests/src/test_book.c", .expected = false },
    .{ .path = "examples/ek_ra8d2/demo/build/generated.c", .expected = false },
    .{ .path = "examples/ek_ra8d2/demo/build-reflow-v2/generated.c", .expected = false },
    .{ .path = "port/esp-hosted/build-mcdc/shim.c", .expected = false },
    .{ .path = "tools/demo/_deps/vendor.c", .expected = false },
    .{ .path = "src/legacy.c", .expected = false },
};

fn fixture(path: []const u8, covered: i64, total: i64) Entry {
    return .{ .file = path, .covered_decisions = covered, .total_decisions = total };
}

/// The five one-decision production fixtures, one per required root.
pub const covered_fixtures = [_]Entry{
    fixture("libs/ra8_core/src/core.c", 1, 1),
    fixture("apps/shared_libs/book/src/book.c", 1, 1),
    fixture("examples/ek_ra8d2/demo/src/main.c", 1, 1),
    fixture("port/posix/src/io.c", 1, 1),
    fixture("tools/ra8_emulator/src/main.c", 1, 1),
};

/// Prove every production root is required and every exemption stays out.
pub fn selftest(allocator: std.mem.Allocator, out: anytype) !u8 {
    var failures = std.ArrayList([]const u8).init(allocator);
    defer {
        for (failures.items) |line| allocator.free(line);
        failures.deinit();
    }

    for (scope_cases) |case| {
        if (implementation.inScope(case.path) != case.expected) {
            try failures.append(try std.fmt.allocPrint(
                allocator,
                "scope mismatch for {s}: expected {s}",
                .{ case.path, if (case.expected) "True" else "False" },
            ));
        }
    }

    {
        var collected = try implementation.collectOffenders(allocator, &covered_fixtures, "ra8-firmware");
        defer collected.deinit(allocator);
        var every_root_once = true;
        for (implementation.in_scope_prefixes) |prefix| {
            if (collected.counts.get(prefix) != 1) every_root_once = false;
        }
        if (collected.offenders.len != 0 or !every_root_once) {
            try failures.append(try allocator.dupe(
                u8,
                "covered fixtures did not populate every required production root",
            ));
        }
    }

    {
        var below = covered_fixtures;
        for (&below) |*entry| {
            if (std.mem.startsWith(u8, entry.file, "port/")) entry.covered_decisions = 0;
        }
        var collected = try implementation.collectOffenders(allocator, &below, "ra8-firmware");
        defer collected.deinit(allocator);
        const one_offender = collected.offenders.len == 1 and
            std.mem.eql(u8, collected.offenders[0].rel, "port/posix/src/io.c");
        if (!one_offender) {
            try failures.append(try allocator.dupe(
                u8,
                "below-floor production fixture did not become an offender",
            ));
        }
    }

    {
        var without_tools = std.ArrayList(Entry).init(allocator);
        defer without_tools.deinit();
        for (covered_fixtures) |entry| {
            if (!std.mem.startsWith(u8, entry.file, "tools/")) try without_tools.append(entry);
        }
        var collected = try implementation.collectOffenders(allocator, without_tools.items, "ra8-firmware");
        defer collected.deinit(allocator);
        const missing = try implementation.missingScopes(allocator, collected.counts);
        defer allocator.free(missing);
        const only_tools = missing.len == 1 and std.mem.eql(u8, missing[0], "tools/");
        if (collected.offenders.len != 0 or !only_tools) {
            try failures.append(try allocator.dupe(
                u8,
                "a missing production root did not fail its non-vacuity check",
            ));
        }
    }

    {
        const vendor_only = [_]Entry{
            fixture("libs/third_party/soup.c", 0, 1),
            fixture("apps/shared_libs/third_party/soup.c", 0, 1),
        };
        var collected = try implementation.collectOffenders(allocator, &vendor_only, "ra8-firmware");
        defer collected.deinit(allocator);
        const missing = try implementation.missingScopes(allocator, collected.counts);
        defer allocator.free(missing);
        if (collected.offenders.len != 0 or missing.len != implementation.in_scope_prefixes.len) {
            try failures.append(try allocator.dupe(
                u8,
                "exempt-only input did not fail every production non-vacuity check",
            ));
        }
    }

    if (failures.items.len != 0) {
        for (failures.items) |line| try out.print("{s} selftest: FAIL -- {s}\n", .{ tool, line });
        return exit_fail;
    }
    try out.print("{s} selftest: PASS -- scope and non-vacuity checks hold.\n", .{tool});
    return exit_ok;
}

/// True when argv selects the selftest, i.e. it is EXACTLY `--selftest`.
pub fn wantsSelftest(argv: []const []const u8) bool {
    return argv.len == 1 and std.mem.eql(u8, argv[0], "--selftest");
}

pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    repo_name: []const u8,
    argv: []const []const u8,
    out: anytype,
) !u8 {
    if (wantsSelftest(argv)) return selftest(allocator, out);
    return gate(allocator, dir, repo_root, repo_name, out);
}
