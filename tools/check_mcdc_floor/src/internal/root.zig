//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure rules for the per-file MC/DC FLOOR gate (#858): path normalisation,
//! first-party scope classification, the reachable-rate arithmetic and the
//! report rendering. No file system, no argv, no process state, so every rule
//! below is exercised directly by the regression suite.
//!
//! The unit of measure is the llvm-cov MC/DC decision region. A file's score
//! is its REACHABLE rate: decisions documented as unreachable on any
//! public-API path (DO-178C 6.4.4.3, catalogued in docs/MCDC_DEACTIVATIONS.md)
//! leave both numerator and denominator, so a file whose only gaps are
//! deactivated stays at 100%.

const std = @import("std");

/// Diagnostic prefix. The predecessor spelled it with its `.py` suffix; every
/// migrated gate in this epic drops the suffix, and nothing parses the line.
pub const tool = "check_mcdc_floor";

/// Per-file reachable-MC/DC floor, in percent. No allowlist and no per-file
/// exemption table: a file below this is fixed at the root.
pub const floor_pct: f64 = 100.0;

/// First-party production roots represented in the live MC/DC report.
pub const in_scope_prefixes = [_][]const u8{
    "libs/",
    "apps/shared_libs/",
    "examples/",
    "port/",
    "tools/",
};

/// Vendored SOUP and generated font tables.
pub const out_of_scope_prefixes = [_][]const u8{
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
};

/// Nested test suites and the dependency-fetch output directory name.
pub const out_of_scope_parts = [_][]const u8{ "tests", "test", "_deps" };

/// True for a CMake/build output directory component.
pub fn isGeneratedPart(part: []const u8) bool {
    return std.mem.eql(u8, part, "build") or std.mem.startsWith(u8, part, "build-");
}

fn hasAnyPrefix(rel: []const u8, prefixes: []const []const u8) bool {
    for (prefixes) |prefix| {
        if (std.mem.startsWith(u8, rel, prefix)) return true;
    }
    return false;
}

/// `pathlib.PurePosixPath(rel).parts` for a relative path: components are
/// split on `/`, empty runs collapse and a bare `.` component disappears,
/// while `..` survives as its own component.
pub const PartIterator = struct {
    rest: []const u8,

    pub fn init(rel: []const u8) PartIterator {
        return .{ .rest = rel };
    }

    pub fn next(self: *PartIterator) ?[]const u8 {
        while (self.rest.len != 0) {
            const end = std.mem.indexOfScalar(u8, self.rest, '/') orelse self.rest.len;
            const part = self.rest[0..end];
            self.rest = if (end == self.rest.len) self.rest[end..] else self.rest[end + 1 ..];
            if (part.len == 0) continue;
            if (std.mem.eql(u8, part, ".")) continue;
            return part;
        }
        return null;
    }
};

/// Normalise a coverage-JSON `file` field to a repo-relative POSIX path.
///
/// The field may arrive absolute or already relative, so both are handled.
/// The absolute-path split marker is derived from the checkout directory
/// basename rather than a hardcoded project name, so the gate strips the
/// prefix correctly from any clone whatever the directory is called. The
/// trailing strip is CPython's `str.lstrip("./")`, which removes EVERY
/// leading `.` or `/` rather than one `./` pair.
pub fn normalize(
    allocator: std.mem.Allocator,
    path: []const u8,
    repo_name: []const u8,
) ![]u8 {
    const slashed = try allocator.alloc(u8, path.len);
    defer allocator.free(slashed);
    for (path, 0..) |byte, index| slashed[index] = if (byte == '\\') '/' else byte;

    var view: []const u8 = slashed;
    const marker = try std.fmt.allocPrint(allocator, "/{s}/", .{repo_name});
    defer allocator.free(marker);
    if (std.mem.indexOf(u8, view, marker)) |at| view = view[at + marker.len ..];

    var start: usize = 0;
    while (start < view.len and (view[start] == '.' or view[start] == '/')) start += 1;
    return allocator.dupe(u8, view[start..]);
}

/// True if `rel` is a first-party file subject to the MC/DC floor.
pub fn inScope(rel: []const u8) bool {
    if (!hasAnyPrefix(rel, &in_scope_prefixes)) return false;
    if (hasAnyPrefix(rel, &out_of_scope_prefixes)) return false;
    var parts = PartIterator.init(rel);
    while (parts.next()) |part| {
        for (out_of_scope_parts) |exempt| {
            if (std.mem.eql(u8, part, exempt)) return false;
        }
        if (isGeneratedPart(part)) return false;
    }
    return true;
}

/// The first-party scope root for `rel`, or null when it is exempt.
pub fn scopePrefix(rel: []const u8) ?[]const u8 {
    if (!inScope(rel)) return null;
    for (in_scope_prefixes) |prefix| {
        if (std.mem.startsWith(u8, rel, prefix)) return prefix;
    }
    return null;
}

/// One per-file row of the coverage report, already decoded.
pub const Entry = struct {
    file: []const u8 = "",
    covered_decisions: i64 = 0,
    total_decisions: i64 = 0,
    deactivated_decisions: i64 = 0,
};

pub const Reachable = struct {
    covered: i64,
    reachable_total: i64,
};

/// Reachable (covered, total) decisions for one file. Deactivated decisions
/// leave the denominator; covered decisions are never deactivated, so the
/// numerator is just the count of decisions at 100% MC/DC.
pub fn fileReachable(entry: Entry) Reachable {
    return .{
        .covered = entry.covered_decisions,
        .reachable_total = entry.total_decisions - entry.deactivated_decisions,
    };
}

pub const Offender = struct {
    pct: f64,
    rel: []const u8,
    covered: i64,
    reachable_total: i64,
};

pub const ScopeCounts = struct {
    counts: [in_scope_prefixes.len]usize = [_]usize{0} ** in_scope_prefixes.len,

    pub fn bump(self: *ScopeCounts, prefix: []const u8) void {
        for (in_scope_prefixes, 0..) |candidate, index| {
            if (std.mem.eql(u8, candidate, prefix)) {
                self.counts[index] += 1;
                return;
            }
        }
    }

    pub fn get(self: ScopeCounts, prefix: []const u8) usize {
        for (in_scope_prefixes, 0..) |candidate, index| {
            if (std.mem.eql(u8, candidate, prefix)) return self.counts[index];
        }
        return 0;
    }

    pub fn total(self: ScopeCounts) usize {
        var sum: usize = 0;
        for (self.counts) |count| sum += count;
        return sum;
    }
};

pub const Collected = struct {
    offenders: []Offender,
    counts: ScopeCounts,

    pub fn deinit(self: *Collected, allocator: std.mem.Allocator) void {
        for (self.offenders) |offender| allocator.free(offender.rel);
        allocator.free(self.offenders);
        self.offenders = &[_]Offender{};
    }
};

fn lessThanOffender(_: void, a: Offender, b: Offender) bool {
    if (a.pct != b.pct) return a.pct < b.pct;
    const order = std.mem.order(u8, a.rel, b.rel);
    if (order != .eq) return order == .lt;
    if (a.covered != b.covered) return a.covered < b.covered;
    return a.reachable_total < b.reachable_total;
}

/// Offenders and per-root counts over the in-scope, decision-bearing files.
///
/// A file with no reachable decision is skipped rather than counted as a
/// pass: it has nothing to measure, and scoring it 100% would dilute the
/// floor. Offenders come back sorted worst first, matching the predecessor's
/// tuple sort (rate, path, covered, reachable).
pub fn collectOffenders(
    allocator: std.mem.Allocator,
    entries: []const Entry,
    repo_name: []const u8,
) !Collected {
    var offenders = std.ArrayList(Offender).init(allocator);
    errdefer {
        for (offenders.items) |offender| allocator.free(offender.rel);
        offenders.deinit();
    }
    var counts = ScopeCounts{};

    for (entries) |entry| {
        const rel = try normalize(allocator, entry.file, repo_name);
        var keep = false;
        defer if (!keep) allocator.free(rel);

        const prefix = scopePrefix(rel) orelse continue;
        const reachable = fileReachable(entry);
        if (reachable.reachable_total <= 0) continue;
        counts.bump(prefix);

        const pct = 100.0 * @as(f64, @floatFromInt(reachable.covered)) /
            @as(f64, @floatFromInt(reachable.reachable_total));
        if (pct < floor_pct) {
            try offenders.append(.{
                .pct = pct,
                .rel = rel,
                .covered = reachable.covered,
                .reachable_total = reachable.reachable_total,
            });
            keep = true;
        }
    }

    const owned = try offenders.toOwnedSlice();
    std.mem.sort(Offender, owned, {}, lessThanOffender);
    return .{ .offenders = owned, .counts = counts };
}

/// Required first-party roots absent from a coverage report, in declaration
/// order.
pub fn missingScopes(allocator: std.mem.Allocator, counts: ScopeCounts) ![][]const u8 {
    var missing = std.ArrayList([]const u8).init(allocator);
    errdefer missing.deinit();
    for (in_scope_prefixes) |prefix| {
        if (counts.get(prefix) == 0) try missing.append(prefix);
    }
    return missing.toOwnedSlice();
}

/// Round `value * scale` to an integer the way CPython's `format` does: on
/// the EXACT binary double, ties to even.
///
/// Zig's own float formatter is not trusted for this. A tie is reachable
/// here, since any rate of the form k/2^n lands on one: 1/16 is exactly
/// 6.25%, which CPython prints as `6.2` and half-away-from-zero prints as
/// `6.3`.
pub fn roundScaledHalfEven(value: f64, scale: u64) i128 {
    if (!std.math.isFinite(value) or value == 0.0) return 0;
    const negative = value < 0.0;
    const magnitude = @abs(value);

    const parts = std.math.frexp(magnitude);
    const mantissa: u64 = @intFromFloat(parts.significand * 9007199254740992.0);
    const exponent: i32 = parts.exponent - 53;

    const scaled: u128 = @as(u128, mantissa) * @as(u128, scale);
    var result: u128 = 0;
    if (exponent >= 0) {
        if (exponent >= 64) return 0;
        result = scaled << @intCast(exponent);
    } else {
        const shift: u32 = @intCast(-exponent);
        if (shift >= 127) return 0;
        const quotient = scaled >> @intCast(shift);
        const remainder = scaled & ((@as(u128, 1) << @intCast(shift)) - 1);
        const half = @as(u128, 1) << @intCast(shift - 1);
        const round_up = remainder > half or (remainder == half and (quotient & 1) == 1);
        result = quotient + @intFromBool(round_up);
    }

    const signed: i128 = @intCast(result);
    return if (negative) -signed else signed;
}

/// `{:.1f}` of a percentage, right-aligned in five columns, as the table
/// column `  mc/dc` expects.
pub fn writePctWidth5(writer: anytype, value: f64) !void {
    var buffer: [64]u8 = undefined;
    const tenths = roundScaledHalfEven(value, 10);
    const magnitude: u128 = @intCast(if (tenths < 0) -tenths else tenths);
    const rendered = try std.fmt.bufPrint(&buffer, "{s}{d}.{d}", .{
        if (tenths < 0) "-" else "",
        magnitude / 10,
        magnitude % 10,
    });
    if (rendered.len < 5) try writer.writeByteNTimes(' ', 5 - rendered.len);
    try writer.writeAll(rendered);
}

/// A decision count in a fixed column. The predecessor's `{:5d}` and
/// `{:<5d}` are written out rather than handed to Zig's alignment syntax, so
/// the two columns the CI log is read in are pinned by this tool's own code.
fn writeCount(writer: anytype, value: i64, width: usize, align_right: bool) !void {
    var buffer: [32]u8 = undefined;
    const rendered = try std.fmt.bufPrint(&buffer, "{d}", .{value});
    const padding = if (rendered.len < width) width - rendered.len else 0;
    if (align_right) try writer.writeByteNTimes(' ', padding);
    try writer.writeAll(rendered);
    if (!align_right) try writer.writeByteNTimes(' ', padding);
}

/// `{:.0f}` of the floor constant, as both headline lines interpolate it.
pub fn writeFloorPct(writer: anytype) !void {
    try writer.print("{d}", .{roundScaledHalfEven(floor_pct, 1)});
}

/// The below-floor table, worst first, with the remedy paragraph.
pub fn writeOffenderReport(writer: anytype, offenders: []const Offender) !void {
    try writer.print("{s}: {d} first-party file(s) below the ", .{ tool, offenders.len });
    try writeFloorPct(writer);
    try writer.writeAll("% reachable-MC/DC floor (NO allowlist):\n");
    try writer.writeAll("  mc/dc  covered/reachable  file\n");
    for (offenders) |offender| {
        try writer.writeAll("  ");
        try writePctWidth5(writer, offender.pct);
        try writer.writeAll("%  ");
        try writeCount(writer, offender.covered, 5, true);
        try writer.writeAll("/");
        try writeCount(writer, offender.reachable_total, 5, false);
        try writer.print("       {s}\n", .{offender.rel});
    }
    try writer.writeAll(
        "Fix each at the root -- add the missing MC/DC vector (N+1 vectors " ++
            "for N conditions; see docs/MCDC.md), or, if the gap is genuinely " ++
            "unreachable on any public-API path, catalogue it with a " ++
            "`// mcdc-deactivated:` rationale per DO-178C 6.4.4.3. Do NOT add " ++
            "an allowlist.\n",
    );
}

/// The passing summary line.
pub fn writePassLine(writer: anytype, checked: usize) !void {
    try writer.print("{s}: PASS -- all {d} first-party file(s) with a reachable decision are >= ", .{ tool, checked });
    try writeFloorPct(writer);
    try writer.writeAll("% MC/DC.\n");
}
