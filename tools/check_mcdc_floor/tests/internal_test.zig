//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression suite for the pure rules of the per-file MC/DC
//! FLOOR gate. Every case below pins observed predecessor behaviour, quirks
//! included, rather than the behaviour a reader might prefer.

const std = @import("std");
const testing = std.testing;
const implementation = @import("implementation");

const repo = "ra8-firmware";

fn normalized(path: []const u8) ![]u8 {
    return implementation.normalize(testing.allocator, path, repo);
}

fn expectNormalized(expected: []const u8, path: []const u8) !void {
    const got = try normalized(path);
    defer testing.allocator.free(got);
    try testing.expectEqualStrings(expected, got);
}

fn partsOf(allocator: std.mem.Allocator, rel: []const u8) ![][]const u8 {
    var list = std.ArrayList([]const u8).init(allocator);
    var it = implementation.PartIterator.init(rel);
    while (it.next()) |part| try list.append(part);
    return list.toOwnedSlice();
}

fn entry(path: []const u8, covered: i64, total: i64) implementation.Entry {
    return .{ .file = path, .covered_decisions = covered, .total_decisions = total };
}

// ---------------------------------------------------------------- constants

test "the tool name drops the predecessor's .py suffix" {
    try testing.expectEqualStrings("check_mcdc_floor", implementation.tool);
}

test "the floor is a hard 100 percent" {
    try testing.expectEqual(@as(f64, 100.0), implementation.floor_pct);
}

test "the five in-scope roots are the live report's production roots" {
    try testing.expectEqual(@as(usize, 5), implementation.in_scope_prefixes.len);
    try testing.expectEqualStrings("libs/", implementation.in_scope_prefixes[0]);
    try testing.expectEqualStrings("apps/shared_libs/", implementation.in_scope_prefixes[1]);
    try testing.expectEqualStrings("examples/", implementation.in_scope_prefixes[2]);
    try testing.expectEqualStrings("port/", implementation.in_scope_prefixes[3]);
    try testing.expectEqualStrings("tools/", implementation.in_scope_prefixes[4]);
}

test "the three out-of-scope prefixes are vendored SOUP and generated fonts" {
    try testing.expectEqual(@as(usize, 3), implementation.out_of_scope_prefixes.len);
    try testing.expectEqualStrings("libs/third_party/", implementation.out_of_scope_prefixes[0]);
    try testing.expectEqualStrings("apps/shared_libs/third_party/", implementation.out_of_scope_prefixes[1]);
    try testing.expectEqualStrings("libs/ra8_fonts/", implementation.out_of_scope_prefixes[2]);
}

test "the exempt path components are the nested suites and _deps" {
    try testing.expectEqual(@as(usize, 3), implementation.out_of_scope_parts.len);
    try testing.expectEqualStrings("tests", implementation.out_of_scope_parts[0]);
    try testing.expectEqualStrings("test", implementation.out_of_scope_parts[1]);
    try testing.expectEqualStrings("_deps", implementation.out_of_scope_parts[2]);
}

// ------------------------------------------------------- generated segments

test "a component named build is generated" {
    try testing.expect(implementation.isGeneratedPart("build"));
}

test "a build- prefixed component is generated" {
    try testing.expect(implementation.isGeneratedPart("build-reflow-v2"));
    try testing.expect(implementation.isGeneratedPart("build-mcdc"));
    try testing.expect(implementation.isGeneratedPart("build-"));
}

test "a component that merely starts with build and no dash is not generated" {
    try testing.expect(!implementation.isGeneratedPart("builder"));
    try testing.expect(!implementation.isGeneratedPart("builds"));
}

test "a component ending in build is not generated" {
    try testing.expect(!implementation.isGeneratedPart("host_build"));
}

test "an empty component is not generated" {
    try testing.expect(!implementation.isGeneratedPart(""));
}

// --------------------------------------------------------------- path parts

test "parts split on slash" {
    const parts = try partsOf(testing.allocator, "libs/ra8_core/src/core.c");
    defer testing.allocator.free(parts);
    try testing.expectEqual(@as(usize, 4), parts.len);
    try testing.expectEqualStrings("libs", parts[0]);
    try testing.expectEqualStrings("core.c", parts[3]);
}

test "repeated slashes collapse like pathlib" {
    const parts = try partsOf(testing.allocator, "libs//ra8_core///src/core.c");
    defer testing.allocator.free(parts);
    try testing.expectEqual(@as(usize, 4), parts.len);
}

test "a trailing slash yields no empty component" {
    const parts = try partsOf(testing.allocator, "libs/ra8_core/");
    defer testing.allocator.free(parts);
    try testing.expectEqual(@as(usize, 2), parts.len);
    try testing.expectEqualStrings("ra8_core", parts[1]);
}

test "a dot component disappears the way pathlib drops it" {
    const parts = try partsOf(testing.allocator, "libs/./ra8_core/src/core.c");
    defer testing.allocator.free(parts);
    try testing.expectEqual(@as(usize, 4), parts.len);
    try testing.expectEqualStrings("ra8_core", parts[1]);
}

test "a double-dot component survives as its own part" {
    const parts = try partsOf(testing.allocator, "libs/../tools/x.c");
    defer testing.allocator.free(parts);
    try testing.expectEqual(@as(usize, 4), parts.len);
    try testing.expectEqualStrings("..", parts[1]);
}

test "an empty path has no parts" {
    const parts = try partsOf(testing.allocator, "");
    defer testing.allocator.free(parts);
    try testing.expectEqual(@as(usize, 0), parts.len);
}

// ---------------------------------------------------------------- normalize

test "a repo-relative path is already normal" {
    try expectNormalized("libs/ra8_core/src/core.c", "libs/ra8_core/src/core.c");
}

test "an absolute path is cut at the checkout directory name" {
    try expectNormalized("libs/ra8_core/src/core.c", "/home/ci/ra8-firmware/libs/ra8_core/src/core.c");
}

test "the marker needs the checkout name between slashes" {
    try expectNormalized(
        "home/ci/ra8-firmware-old/libs/core.c",
        "/home/ci/ra8-firmware-old/libs/core.c",
    );
}

test "only the first occurrence of the marker splits the path" {
    try expectNormalized(
        "work/ra8-firmware/libs/core.c",
        "/src/ra8-firmware/work/ra8-firmware/libs/core.c",
    );
}

test "backslashes become slashes before the marker is sought" {
    try expectNormalized("libs/ra8_core/src/core.c", "C:\\build\\ra8-firmware\\libs\\ra8_core\\src\\core.c");
}

test "a leading ./ is stripped" {
    try expectNormalized("libs/core.c", "./libs/core.c");
}

test "lstrip removes every leading dot and slash, not one pair" {
    try expectNormalized("libs/core.c", ".././/libs/core.c");
}

test "a leading slash alone is stripped, so an unmatched absolute path becomes relative" {
    try expectNormalized("opt/vendor/soup.c", "/opt/vendor/soup.c");
}

test "an interior dot segment is left for the part iterator" {
    try expectNormalized("libs/./core.c", "libs/./core.c");
}

test "an empty file field normalises to empty" {
    try expectNormalized("", "");
}

test "a path that is only dots and slashes normalises to empty" {
    try expectNormalized("", "./././");
}

test "the checkout name is honoured verbatim, so another clone name still cuts" {
    const got = try implementation.normalize(testing.allocator, "/w/ra8/libs/core.c", "ra8");
    defer testing.allocator.free(got);
    try testing.expectEqualStrings("libs/core.c", got);
}

// ------------------------------------------------------------------- scope

test "each production root is in scope" {
    try testing.expect(implementation.inScope("libs/ra8_core/src/core.c"));
    try testing.expect(implementation.inScope("apps/shared_libs/book/src/book.c"));
    try testing.expect(implementation.inScope("examples/ek_ra8d2/demo/src/main.c"));
    try testing.expect(implementation.inScope("port/posix/src/io.c"));
    try testing.expect(implementation.inScope("tools/ra8_emulator/src/main.c"));
}

test "a path outside every production root is out of scope" {
    try testing.expect(!implementation.inScope("src/legacy.c"));
    try testing.expect(!implementation.inScope("scripts/checks/x.py"));
    try testing.expect(!implementation.inScope(""));
}

test "apps/ alone is not a production root" {
    try testing.expect(!implementation.inScope("apps/demo/src/main.c"));
}

test "vendored SOUP under libs is exempt" {
    try testing.expect(!implementation.inScope("libs/third_party/soup.c"));
}

test "vendored SOUP under apps/shared_libs is exempt" {
    try testing.expect(!implementation.inScope("apps/shared_libs/third_party/soup/source.c"));
}

test "generated font tables are exempt" {
    try testing.expect(!implementation.inScope("libs/ra8_fonts/src/generated.c"));
}

test "a library whose name merely starts with the font prefix stays in scope" {
    try testing.expect(implementation.inScope("libs/ra8_fonts_util/src/util.c"));
}

test "a third_party component that is not the anchored prefix stays in scope" {
    try testing.expect(implementation.inScope("libs/ra8_core/third_party/vendor.c"));
}

test "a nested tests directory is exempt" {
    try testing.expect(!implementation.inScope("apps/shared_libs/book/tests/src/test_book.c"));
}

test "a nested test directory is exempt" {
    try testing.expect(!implementation.inScope("libs/ra8_core/test/case.c"));
}

test "a component named _deps is exempt" {
    try testing.expect(!implementation.inScope("tools/demo/_deps/vendor.c"));
}

test "a build output directory is exempt" {
    try testing.expect(!implementation.inScope("examples/ek_ra8d2/demo/build/generated.c"));
}

test "a build- variant directory is exempt" {
    try testing.expect(!implementation.inScope("examples/ek_ra8d2/demo/build-reflow-v2/generated.c"));
    try testing.expect(!implementation.inScope("port/esp-hosted/build-mcdc/shim.c"));
}

test "a file named build.c is not a build directory" {
    try testing.expect(implementation.inScope("libs/ra8_core/src/build.c"));
}

test "a directory named testsuite is not the exempt component tests" {
    try testing.expect(implementation.inScope("libs/ra8_core/testsuite/case.c"));
}

test "a file literally named tests with no extension is still an exempt component" {
    try testing.expect(!implementation.inScope("libs/ra8_core/src/tests"));
}

test "an interior dot component cannot hide an exempt component" {
    try testing.expect(!implementation.inScope("libs/./tests/case.c"));
}

// ------------------------------------------------------------ scope prefix

test "scope prefix returns the matching root" {
    try testing.expectEqualStrings("libs/", implementation.scopePrefix("libs/ra8_core/src/core.c").?);
    try testing.expectEqualStrings("port/", implementation.scopePrefix("port/posix/src/io.c").?);
    try testing.expectEqualStrings("tools/", implementation.scopePrefix("tools/x/src/main.c").?);
}

test "apps/shared_libs wins over no other root, and libs/ does not match it" {
    const prefix = implementation.scopePrefix("apps/shared_libs/book/src/book.c").?;
    try testing.expectEqualStrings("apps/shared_libs/", prefix);
}

test "an exempt path has no scope prefix" {
    try testing.expect(implementation.scopePrefix("libs/third_party/soup.c") == null);
    try testing.expect(implementation.scopePrefix("src/legacy.c") == null);
}

// -------------------------------------------------------- reachable counts

test "reachable total subtracts deactivated decisions" {
    const reachable = implementation.fileReachable(.{
        .file = "libs/a.c",
        .covered_decisions = 3,
        .total_decisions = 5,
        .deactivated_decisions = 2,
    });
    try testing.expectEqual(@as(i64, 3), reachable.covered);
    try testing.expectEqual(@as(i64, 3), reachable.reachable_total);
}

test "a file whose gaps are all deactivated is exactly at the floor" {
    const reachable = implementation.fileReachable(.{
        .file = "libs/a.c",
        .covered_decisions = 4,
        .total_decisions = 7,
        .deactivated_decisions = 3,
    });
    try testing.expectEqual(@as(i64, 4), reachable.covered);
    try testing.expectEqual(@as(i64, 4), reachable.reachable_total);
}

test "missing counts default to zero" {
    const reachable = implementation.fileReachable(.{ .file = "libs/a.c" });
    try testing.expectEqual(@as(i64, 0), reachable.covered);
    try testing.expectEqual(@as(i64, 0), reachable.reachable_total);
}

test "deactivating every decision leaves nothing reachable" {
    const reachable = implementation.fileReachable(.{
        .file = "libs/a.c",
        .covered_decisions = 0,
        .total_decisions = 4,
        .deactivated_decisions = 4,
    });
    try testing.expectEqual(@as(i64, 0), reachable.reachable_total);
}

// ------------------------------------------------------ collecting offenders

test "a covered file is no offender and counts toward its root" {
    const entries = [_]implementation.Entry{entry("libs/ra8_core/src/core.c", 4, 4)};
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 0), collected.offenders.len);
    try testing.expectEqual(@as(usize, 1), collected.counts.get("libs/"));
    try testing.expectEqual(@as(usize, 1), collected.counts.total());
}

test "a file below the floor becomes an offender" {
    const entries = [_]implementation.Entry{entry("libs/ra8_core/src/core.c", 3, 4)};
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 1), collected.offenders.len);
    try testing.expectEqualStrings("libs/ra8_core/src/core.c", collected.offenders[0].rel);
    try testing.expectEqual(@as(i64, 3), collected.offenders[0].covered);
    try testing.expectEqual(@as(i64, 4), collected.offenders[0].reachable_total);
    try testing.expectEqual(@as(f64, 75.0), collected.offenders[0].pct);
}

test "a file with no reachable decision is skipped rather than passed" {
    const entries = [_]implementation.Entry{
        .{ .file = "libs/a.c", .covered_decisions = 0, .total_decisions = 0 },
        .{ .file = "libs/b.c", .covered_decisions = 0, .total_decisions = 2, .deactivated_decisions = 2 },
    };
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 0), collected.offenders.len);
    try testing.expectEqual(@as(usize, 0), collected.counts.total());
}

test "a negative reachable total is skipped, not treated as an offender" {
    const entries = [_]implementation.Entry{
        .{ .file = "libs/a.c", .covered_decisions = 0, .total_decisions = 1, .deactivated_decisions = 3 },
    };
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 0), collected.counts.total());
}

test "an exempt file is neither counted nor judged" {
    const entries = [_]implementation.Entry{
        entry("libs/third_party/soup.c", 0, 4),
        entry("apps/shared_libs/book/tests/src/test_book.c", 0, 9),
    };
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 0), collected.offenders.len);
    try testing.expectEqual(@as(usize, 0), collected.counts.total());
}

test "an absolute file field is normalised before scope is judged" {
    const entries = [_]implementation.Entry{
        entry("/home/ci/ra8-firmware/libs/ra8_core/src/core.c", 1, 2),
    };
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 1), collected.offenders.len);
    try testing.expectEqualStrings("libs/ra8_core/src/core.c", collected.offenders[0].rel);
}

test "over-covered files are not offenders" {
    const entries = [_]implementation.Entry{entry("libs/a.c", 5, 4)};
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 0), collected.offenders.len);
    try testing.expectEqual(@as(usize, 1), collected.counts.total());
}

test "counts accumulate per root over many files" {
    const entries = [_]implementation.Entry{
        entry("libs/a.c", 1, 1),
        entry("libs/b.c", 1, 1),
        entry("port/posix/src/io.c", 1, 1),
    };
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 2), collected.counts.get("libs/"));
    try testing.expectEqual(@as(usize, 1), collected.counts.get("port/"));
    try testing.expectEqual(@as(usize, 0), collected.counts.get("tools/"));
    try testing.expectEqual(@as(usize, 3), collected.counts.total());
}

test "offenders sort worst first" {
    const entries = [_]implementation.Entry{
        entry("libs/mid.c", 1, 2),
        entry("libs/worst.c", 0, 4),
        entry("libs/near.c", 9, 10),
    };
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 3), collected.offenders.len);
    try testing.expectEqualStrings("libs/worst.c", collected.offenders[0].rel);
    try testing.expectEqualStrings("libs/mid.c", collected.offenders[1].rel);
    try testing.expectEqualStrings("libs/near.c", collected.offenders[2].rel);
}

test "offenders at the same rate sort by path in code-point order" {
    const entries = [_]implementation.Entry{
        entry("libs/zeta.c", 1, 2),
        entry("libs/Alpha.c", 1, 2),
        entry("libs/alpha.c", 1, 2),
    };
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqualStrings("libs/Alpha.c", collected.offenders[0].rel);
    try testing.expectEqualStrings("libs/alpha.c", collected.offenders[1].rel);
    try testing.expectEqualStrings("libs/zeta.c", collected.offenders[2].rel);
}

test "two files that normalise to the same path both stay offenders" {
    const entries = [_]implementation.Entry{
        entry("/x/ra8-firmware/libs/a.c", 1, 2),
        entry("libs/a.c", 0, 2),
    };
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 2), collected.offenders.len);
    try testing.expectEqual(@as(i64, 0), collected.offenders[0].covered);
}

test "an empty input yields no offenders and no counts" {
    const entries = [_]implementation.Entry{};
    var collected = try implementation.collectOffenders(testing.allocator, &entries, repo);
    defer collected.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 0), collected.offenders.len);
    try testing.expectEqual(@as(usize, 0), collected.counts.total());
}

// -------------------------------------------------------- missing scopes

test "every root missing is reported in declaration order" {
    const counts = implementation.ScopeCounts{};
    const missing = try implementation.missingScopes(testing.allocator, counts);
    defer testing.allocator.free(missing);
    try testing.expectEqual(@as(usize, 5), missing.len);
    try testing.expectEqualStrings("libs/", missing[0]);
    try testing.expectEqualStrings("tools/", missing[4]);
}

test "a populated root drops out of the missing list" {
    var counts = implementation.ScopeCounts{};
    counts.bump("libs/");
    counts.bump("apps/shared_libs/");
    counts.bump("examples/");
    counts.bump("port/");
    const missing = try implementation.missingScopes(testing.allocator, counts);
    defer testing.allocator.free(missing);
    try testing.expectEqual(@as(usize, 1), missing.len);
    try testing.expectEqualStrings("tools/", missing[0]);
}

test "a full sweep has no missing scope" {
    var counts = implementation.ScopeCounts{};
    for (implementation.in_scope_prefixes) |prefix| counts.bump(prefix);
    const missing = try implementation.missingScopes(testing.allocator, counts);
    defer testing.allocator.free(missing);
    try testing.expectEqual(@as(usize, 0), missing.len);
}

test "bumping an unknown prefix changes nothing" {
    var counts = implementation.ScopeCounts{};
    counts.bump("scripts/");
    try testing.expectEqual(@as(usize, 0), counts.total());
}

// ------------------------------------------------------ half-even rounding

test "a tie at the tenths digit rounds to even, as CPython prints it" {
    try testing.expectEqual(@as(i128, 62), implementation.roundScaledHalfEven(6.25, 10));
    try testing.expectEqual(@as(i128, 68), implementation.roundScaledHalfEven(6.75, 10));
    try testing.expectEqual(@as(i128, 2), implementation.roundScaledHalfEven(0.25, 10));
}

test "a non-tie rounds to nearest" {
    try testing.expectEqual(@as(i128, 126), implementation.roundScaledHalfEven(12.56, 10));
    try testing.expectEqual(@as(i128, 125), implementation.roundScaledHalfEven(12.54, 10));
}

test "exact tenths are unchanged" {
    try testing.expectEqual(@as(i128, 500), implementation.roundScaledHalfEven(50.0, 10));
    try testing.expectEqual(@as(i128, 1000), implementation.roundScaledHalfEven(100.0, 10));
    try testing.expectEqual(@as(i128, 0), implementation.roundScaledHalfEven(0.0, 10));
}

test "scaling by one renders the floor constant" {
    try testing.expectEqual(@as(i128, 100), implementation.roundScaledHalfEven(implementation.floor_pct, 1));
}

test "a negative value keeps its sign through the rounding" {
    try testing.expectEqual(@as(i128, -62), implementation.roundScaledHalfEven(-6.25, 10));
}

test "a non-finite value rounds to zero rather than trapping" {
    try testing.expectEqual(@as(i128, 0), implementation.roundScaledHalfEven(std.math.inf(f64), 10));
    try testing.expectEqual(@as(i128, 0), implementation.roundScaledHalfEven(std.math.nan(f64), 10));
}

test "a value far below half a tenth rounds to zero" {
    try testing.expectEqual(@as(i128, 0), implementation.roundScaledHalfEven(1e-300, 10));
}

test "the sixteenth rate is the tie the naive formatter would get wrong" {
    const pct = 100.0 * 1.0 / 16.0;
    try testing.expectEqual(@as(f64, 6.25), pct);
    try testing.expectEqual(@as(i128, 62), implementation.roundScaledHalfEven(pct, 10));
}

// ------------------------------------------------------------- rendering

fn renderPct(buffer: []u8, value: f64) ![]const u8 {
    var stream = std.io.fixedBufferStream(buffer);
    try implementation.writePctWidth5(stream.writer(), value);
    return stream.getWritten();
}

test "a percentage is right-aligned in five columns" {
    var buffer: [32]u8 = undefined;
    try testing.expectEqualStrings("100.0", try renderPct(&buffer, 100.0));
    try testing.expectEqualStrings(" 75.0", try renderPct(&buffer, 75.0));
    try testing.expectEqualStrings("  0.0", try renderPct(&buffer, 0.0));
    try testing.expectEqualStrings("  6.2", try renderPct(&buffer, 6.25));
}

test "the floor renders without a decimal point" {
    var buffer: [16]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    try implementation.writeFloorPct(stream.writer());
    try testing.expectEqualStrings("100", stream.getWritten());
}

test "the offender report carries the header, the table and the remedy" {
    var buffer: [4096]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    const offenders = [_]implementation.Offender{
        .{ .pct = 0.0, .rel = "libs/a.c", .covered = 0, .reachable_total = 3 },
        .{ .pct = 75.0, .rel = "port/posix/src/io.c", .covered = 3, .reachable_total = 4 },
    };
    try implementation.writeOffenderReport(stream.writer(), &offenders);
    const text = stream.getWritten();
    try testing.expect(std.mem.startsWith(
        u8,
        text,
        "check_mcdc_floor: 2 first-party file(s) below the 100% reachable-MC/DC floor (NO allowlist):\n",
    ));
    try testing.expect(std.mem.indexOf(u8, text, "  mc/dc  covered/reachable  file\n") != null);
    try testing.expect(std.mem.indexOf(u8, text, "    0.0%      0/3           libs/a.c\n") != null);
    try testing.expect(std.mem.indexOf(u8, text, "   75.0%      3/4           port/posix/src/io.c\n") != null);
    try testing.expect(std.mem.indexOf(u8, text, "// mcdc-deactivated:") != null);
    try testing.expect(std.mem.endsWith(u8, text, "Do NOT add an allowlist.\n"));
}

test "a wide reachable count still left-aligns in its column" {
    var buffer: [2048]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    const offenders = [_]implementation.Offender{
        .{ .pct = 99.9, .rel = "libs/big.c", .covered = 123456, .reachable_total = 123457 },
    };
    try implementation.writeOffenderReport(stream.writer(), &offenders);
    try testing.expect(std.mem.indexOf(u8, stream.getWritten(), "123456/123457       libs/big.c\n") != null);
}

test "the pass line names the checked count and the floor" {
    var buffer: [512]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    try implementation.writePassLine(stream.writer(), 312);
    try testing.expectEqualStrings(
        "check_mcdc_floor: PASS -- all 312 first-party file(s) with a reachable decision are >= 100% MC/DC.\n",
        stream.getWritten(),
    );
}

test "an empty offender list still renders a well-formed header" {
    var buffer: [2048]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    try implementation.writeOffenderReport(stream.writer(), &[_]implementation.Offender{});
    try testing.expect(std.mem.indexOf(u8, stream.getWritten(), ": 0 first-party file(s) below") != null);
}
