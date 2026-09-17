//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure computation behind the per-file MC/DC floor gate (#858, #1205).
//! Nothing here opens a file, reads the environment or looks at argv: this
//! module turns one already-parsed coverage document into the offender table,
//! the per-root census and the exact lines the predecessor printed.
//!
//! The predecessor, scripts/checks/check_mcdc_floor.py, measured a file's
//! REACHABLE MC/DC rate, `covered / (total - deactivated)`, and failed CI when
//! any first-party file with at least one reachable decision sat below the
//! floor. Three of its properties are load-bearing and pinned here rather than
//! tidied: a file with no reachable decision is SKIPPED (scoring it 100% would
//! dilute the floor), every production root must contribute at least one
//! reachable decision (so a missing report subtree cannot pass vacuously), and
//! there is NO allowlist anywhere in the data path.
//!
//! Two CPython behaviours are reproduced deliberately, because the report is
//! machine-written and its fields are not validated upstream:
//!
//!   * `int(value)` coercion, so a JSON float truncates toward zero, a bool is
//!     1 or 0, a numeric string parses (underscores, signs, surrounding ASCII
//!     whitespace and non-ASCII decimal digits included) and anything else is
//!     the TypeError the predecessor died on.
//!   * `%5.1f` rounding, which is round-HALF-EVEN on the exact binary value.
//!     `100 * 1 / 16` is exactly 6.25 and prints "  6.2", where a half-up
//!     formatter prints "  6.3". The offender table is read by humans out of
//!     the CI log, so the digits are part of the contract.
//!
//! Integers are held in `i128` rather than CPython's unbounded `int`. A field
//! whose magnitude exceeds that is reported as an unreadable document instead
//! of being wrapped: no llvm-cov report can produce one, and wrapping would
//! turn a nonsense field into a silent pass.

const std = @import("std");
const char_classes = @import("char_classes.zig");

/// Program name in every rendered line. The predecessor spelled itself
/// `check_mcdc_floor.py`; the migrated tool drops the suffix.
pub const tool_name = "check_mcdc_floor";

/// Per-file reachable-MC/DC floor, in percent. DO-178C Level B mandates full
/// MC/DC of every reachable compound decision, so a reachable gap in a single
/// file is a hard failure. No allowlist: a file below this is fixed at the
/// root, never grandfathered.
pub const floor_pct: f64 = 100.0;

/// Repo-relative coverage document, written by the MC/DC regenerator.
pub const mcdc_json_rel = "build/mcdc-report/mcdc_per_file.json";

/// First-party production roots represented in the live MC/DC report. Order is
/// load-bearing twice: `scopePrefix` returns the FIRST match, and the
/// non-vacuity message lists missing roots in this order.
pub const in_scope_prefixes = [_][]const u8{
    "libs/",
    "apps/shared_libs/",
    "examples/",
    "port/",
    "tools/",
};

/// Vendored SOUP and generated font tables: exempt from first-party rules, so
/// exempt from the floor too.
pub const out_of_scope_prefixes = [_][]const u8{
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
};

/// Nested test suites and the dependency-fetch output directory name.
pub const out_of_scope_parts = [_][]const u8{ "tests", "test", "_deps" };

/// One below-floor file, in the predecessor's tuple order so the sort matches.
pub const Offender = struct {
    pct: f64,
    rel: []const u8,
    covered: i128,
    reachable_total: i128,
};

/// Per-root count of in-scope files carrying at least one reachable decision.
pub const Census = [in_scope_prefixes.len]usize;

/// What a malformed document does to the predecessor, by the exception it
/// raised: `type_error` and `attribute_error` died with a traceback on stderr,
/// `value_error` likewise, and `out_of_range` is this tool's documented bound.
pub const FieldError = error{
    TypeError,
    ValueError,
    AttributeError,
    OutOfRange,
};

pub const CoerceError = FieldError || error{OutOfMemory};

/// True for a CMake/build output directory component.
pub fn isGeneratedPart(part: []const u8) bool {
    return std.mem.eql(u8, part, "build") or std.mem.startsWith(u8, part, "build-");
}

fn startsWithAny(rel: []const u8, prefixes: []const []const u8) bool {
    for (prefixes) |prefix| {
        if (std.mem.startsWith(u8, rel, prefix)) return true;
    }
    return false;
}

/// `pathlib.PurePosixPath(rel).parts` for the relative paths this gate sees:
/// empty components and "." are dropped, ".." is kept verbatim.
///
/// A rooted path would gain pathlib's "/" root component, but one can never
/// reach the parts walk: `inScope` rejects it at the prefix test first, since
/// no in-scope prefix starts with a separator.
pub const PartsIterator = struct {
    rest: []const u8,

    pub fn next(self: *PartsIterator) ?[]const u8 {
        while (self.rest.len != 0) {
            const end = std.mem.indexOfScalar(u8, self.rest, '/') orelse self.rest.len;
            const part = self.rest[0..end];
            self.rest = if (end == self.rest.len) self.rest[end..] else self.rest[end + 1 ..];
            if (part.len == 0 or std.mem.eql(u8, part, ".")) continue;
            return part;
        }
        return null;
    }
};

pub fn partsIterator(rel: []const u8) PartsIterator {
    return .{ .rest = rel };
}

/// Normalise a coverage-JSON `file` field to a repo-relative POSIX path.
///
/// The absolute-path split marker is the checkout directory basename, not a
/// hardcoded project name, so the prefix is stripped correctly from any clone.
/// Three details are the predecessor's, kept exactly: backslashes fold to
/// forward slashes first (so a Windows-shaped path normalises), only the FIRST
/// occurrence of the marker splits (so a nested directory of the same name
/// stays in the result), and the leading strip is `lstrip("./")`, a character
/// set rather than a prefix, so "../libs/a.c" and "...libs/a.c" both normalise
/// to "libs/a.c".
///
/// The result borrows from `allocator`; callers pass an arena.
pub fn normalize(allocator: std.mem.Allocator, path: []const u8, root_name: []const u8) ![]const u8 {
    const slashed = try allocator.alloc(u8, path.len);
    for (path, 0..) |byte, index| slashed[index] = if (byte == '\\') '/' else byte;

    const marker = try std.fmt.allocPrint(allocator, "/{s}/", .{root_name});
    var rest: []const u8 = slashed;
    if (std.mem.indexOf(u8, slashed, marker)) |at| rest = slashed[at + marker.len ..];

    var start: usize = 0;
    while (start < rest.len and (rest[start] == '.' or rest[start] == '/')) start += 1;
    return rest[start..];
}

/// True if `rel` is a first-party file subject to the MC/DC floor.
pub fn inScope(rel: []const u8) bool {
    if (!startsWithAny(rel, &in_scope_prefixes)) return false;
    if (startsWithAny(rel, &out_of_scope_prefixes)) return false;
    var parts = partsIterator(rel);
    while (parts.next()) |part| {
        for (out_of_scope_parts) |excluded| {
            if (std.mem.eql(u8, part, excluded)) return false;
        }
        if (isGeneratedPart(part)) return false;
    }
    return true;
}

/// Index into `in_scope_prefixes` for `rel`, or null when it is exempt.
pub fn scopeIndex(rel: []const u8) ?usize {
    if (!inScope(rel)) return null;
    for (in_scope_prefixes, 0..) |prefix, index| {
        if (std.mem.startsWith(u8, rel, prefix)) return index;
    }
    unreachable;
}

/// The first-party scope root for `rel`, or null when it is exempt.
pub fn scopePrefix(rel: []const u8) ?[]const u8 {
    const index = scopeIndex(rel) orelse return null;
    return in_scope_prefixes[index];
}

/// CPython's `int(value)` over one JSON value.
pub fn pythonInt(allocator: std.mem.Allocator, value: std.json.Value) CoerceError!i128 {
    return switch (value) {
        .integer => |integer| @as(i128, integer),
        .bool => |flag| @as(i128, if (flag) 1 else 0),
        .float => |float| floatToInt(float),
        .number_string => |text| parseIntText(allocator, text) catch error.OutOfRange,
        .string => |text| parseIntText(allocator, text),
        .null, .array, .object => error.TypeError,
    };
}

fn floatToInt(float: f64) FieldError!i128 {
    if (std.math.isNan(float) or std.math.isInf(float)) return error.ValueError;
    const truncated = @trunc(float);
    if (truncated > 1.7e38 or truncated < -1.7e38) return error.OutOfRange;
    return @as(i128, @intFromFloat(truncated));
}

/// CPython's `int(str)`: the text is first rewritten by
/// `_PyUnicode_TransformDecimalAndSpaceToASCII`, then parsed as ASCII with
/// optional surrounding whitespace, an optional sign and single underscores
/// between digits.
fn parseIntText(allocator: std.mem.Allocator, text: []const u8) CoerceError!i128 {
    const transformed = try transformDecimalAndSpace(allocator, text);
    const trimmed = std.mem.trim(u8, transformed, " \t\n\r\x0b\x0c");
    if (trimmed.len == 0) return error.ValueError;

    var index: usize = 0;
    var negative = false;
    if (trimmed[0] == '+' or trimmed[0] == '-') {
        negative = trimmed[0] == '-';
        index = 1;
    }
    if (index == trimmed.len) return error.ValueError;

    var magnitude: i128 = 0;
    var previous_underscore = true;
    var digits: usize = 0;
    while (index < trimmed.len) : (index += 1) {
        const byte = trimmed[index];
        if (byte == '_') {
            if (previous_underscore) return error.ValueError;
            previous_underscore = true;
            continue;
        }
        if (byte < '0' or byte > '9') return error.ValueError;
        previous_underscore = false;
        digits += 1;
        magnitude = std.math.mul(i128, magnitude, 10) catch return error.OutOfRange;
        magnitude = std.math.add(i128, magnitude, @as(i128, byte - '0')) catch return error.OutOfRange;
    }
    if (digits == 0 or previous_underscore) return error.ValueError;
    return if (negative) -magnitude else magnitude;
}

fn transformDecimalAndSpace(allocator: std.mem.Allocator, text: []const u8) error{OutOfMemory}![]const u8 {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();

    var index: usize = 0;
    while (index < text.len) {
        const byte = text[index];
        if (byte < 0x80) {
            try out.append(byte);
            index += 1;
            continue;
        }
        const length = std.unicode.utf8ByteSequenceLength(byte) catch {
            try out.append(byte);
            index += 1;
            continue;
        };
        if (index + length > text.len) {
            try out.append(byte);
            index += 1;
            continue;
        }
        const code_point = std.unicode.utf8Decode(text[index .. index + length]) catch {
            try out.append(byte);
            index += 1;
            continue;
        };
        index += length;
        if (isNonAsciiSpace(code_point)) {
            try out.append(' ');
        } else if (nonAsciiDigitValue(code_point)) |digit| {
            try out.append('0' + digit);
        } else {
            try out.appendSlice(text[index - length .. index]);
        }
    }
    return out.toOwnedSlice();
}

fn isNonAsciiSpace(code_point: u21) bool {
    for (char_classes.non_ascii_space_intervals) |interval| {
        if (code_point >= interval[0] and code_point <= interval[1]) return true;
    }
    return false;
}

fn nonAsciiDigitValue(code_point: u21) ?u8 {
    for (char_classes.non_ascii_digit_run_starts) |start| {
        if (code_point >= start and code_point <= start + 9) {
            return @intCast(code_point - start);
        }
    }
    return null;
}

/// One entry's `(covered, reachable_total)` decision counts.
///
/// Deactivated decisions (DO-178C 6.4.4.3) come out of the denominator only;
/// a covered decision is never deactivated, so the numerator is just the count
/// of decisions at 100% MC/DC. Field order is the predecessor's, because it
/// decides WHICH malformed field is the one that kills the run.
pub const Reach = struct { covered: i128, reachable_total: i128 };

pub fn fileReachable(allocator: std.mem.Allocator, entry: std.json.Value) CoerceError!Reach {
    const object = switch (entry) {
        .object => |object| object,
        else => return error.AttributeError,
    };
    const total = try intField(allocator, object, "total_decisions");
    const covered = try intField(allocator, object, "covered_decisions");
    const deactivated = try intField(allocator, object, "deactivated_decisions");
    const reachable_total = std.math.sub(i128, total, deactivated) catch return error.OutOfRange;
    return .{ .covered = covered, .reachable_total = reachable_total };
}

fn intField(allocator: std.mem.Allocator, object: std.json.ObjectMap, key: []const u8) CoerceError!i128 {
    const value = object.get(key) orelse return 0;
    return pythonInt(allocator, value);
}

/// The `file` field as the predecessor read it: a missing field is the empty
/// string (which normalises to itself and falls out of scope), and a non-string
/// field is the AttributeError `str.replace` raised on it.
///
/// `number_string` is a NUMBER, not a path. The parser only produces that
/// variant for a numeric token too large for an i64, never for a quoted
/// string, so it takes the same AttributeError branch every other number
/// does: `json.load` handed the predecessor an `int` here and `path.replace`
/// raised on it. Reading its text as a path instead put the entry out of
/// scope and skipped it in silence, which is the one outcome this floor
/// exists to prevent.
pub fn fileField(entry: std.json.Value) FieldError![]const u8 {
    const object = switch (entry) {
        .object => |object| object,
        else => return error.AttributeError,
    };
    const value = object.get("file") orelse return "";
    return switch (value) {
        .string => |text| text,
        else => error.AttributeError,
    };
}

/// `100.0 * covered / reachable_total`, in the predecessor's operation order.
pub fn reachablePct(covered: i128, reachable_total: i128) f64 {
    return 100.0 * @as(f64, @floatFromInt(covered)) / @as(f64, @floatFromInt(reachable_total));
}

/// The predecessor's `offenders.sort()`: tuples compare left to right, so the
/// worst rate leads and equal rates fall back to the path's code-point order.
pub fn offenderLessThan(_: void, left: Offender, right: Offender) bool {
    if (left.pct != right.pct) return left.pct < right.pct;
    const order = std.mem.order(u8, left.rel, right.rel);
    if (order != .eq) return order == .lt;
    if (left.covered != right.covered) return left.covered < right.covered;
    return left.reachable_total < right.reachable_total;
}

pub const Collected = struct {
    offenders: []Offender,
    census: Census,

    pub fn checked(self: Collected) usize {
        var total: usize = 0;
        for (self.census) |count| total += count;
        return total;
    }
};

/// Offenders and the per-root census over the document's `files` entries.
///
/// A file with no reachable decision is skipped rather than counted as a pass:
/// it has nothing to measure, and scoring it 100% would dilute the floor.
pub fn collectOffenders(
    allocator: std.mem.Allocator,
    files: []const std.json.Value,
    root_name: []const u8,
) CoerceError!Collected {
    var offenders = std.ArrayList(Offender).init(allocator);
    errdefer offenders.deinit();
    var census: Census = [_]usize{0} ** in_scope_prefixes.len;

    for (files) |entry| {
        const rel = try normalize(allocator, try fileField(entry), root_name);
        const index = scopeIndex(rel) orelse continue;
        const reach = try fileReachable(allocator, entry);
        if (reach.reachable_total <= 0) continue;
        census[index] += 1;
        const pct = reachablePct(reach.covered, reach.reachable_total);
        if (pct < floor_pct) {
            try offenders.append(.{
                .pct = pct,
                .rel = rel,
                .covered = reach.covered,
                .reachable_total = reach.reachable_total,
            });
        }
    }

    const owned = try offenders.toOwnedSlice();
    std.mem.sort(Offender, owned, {}, offenderLessThan);
    return .{ .offenders = owned, .census = census };
}

/// Required production roots absent from a coverage report, in declared order.
pub fn missingScopes(allocator: std.mem.Allocator, census: Census) ![]const []const u8 {
    var missing = std.ArrayList([]const u8).init(allocator);
    errdefer missing.deinit();
    for (in_scope_prefixes, 0..) |prefix, index| {
        if (census[index] == 0) try missing.append(prefix);
    }
    return missing.toOwnedSlice();
}

/// CPython's `%5.1f`, which rounds the EXACT binary value half to even.
///
/// Zig's float formatter rounds a tie away from zero, so 6.25 would print
/// "  6.3" where CPython prints "  6.2". The exact decimal expansion of a
/// double is finite, so this formats it with enough digits to see past the
/// rounding position and then rounds the digit string itself.
pub fn formatPct(buffer: []u8, value: f64) ![]const u8 {
    var wide: [512]u8 = undefined;
    const exact = try std.fmt.bufPrint(&wide, "{d:.24}", .{value});

    const negative = exact.len != 0 and exact[0] == '-';
    const body = if (negative) exact[1..] else exact;
    const point = std.mem.indexOfScalar(u8, body, '.') orelse body.len;

    var digits: [512]u8 = undefined;
    var count: usize = 0;
    for (body) |byte| {
        if (byte == '.') continue;
        digits[count] = byte;
        count += 1;
    }

    // Keep one fractional digit; decide the rounding from everything after it.
    const keep = point + 1;
    var round_up = false;
    if (keep < count) {
        const next = digits[keep];
        if (next > '5') {
            round_up = true;
        } else if (next == '5') {
            var rest_nonzero = false;
            for (digits[keep + 1 .. count]) |byte| {
                if (byte != '0') rest_nonzero = true;
            }
            round_up = rest_nonzero or (digits[keep - 1] - '0') % 2 == 1;
        }
        count = keep;
    }

    if (round_up) {
        var index = count;
        while (index > 0) {
            index -= 1;
            if (digits[index] == '9') {
                digits[index] = '0';
                continue;
            }
            digits[index] += 1;
            break;
        } else {
            std.mem.copyBackwards(u8, digits[1 .. count + 1], digits[0..count]);
            digits[0] = '1';
            count += 1;
            return renderFixed(buffer, negative, digits[0..count], point + 1);
        }
    }

    return renderFixed(buffer, negative, digits[0..count], point);
}

fn renderFixed(buffer: []u8, negative: bool, digits: []const u8, integer_digits: usize) ![]const u8 {
    var text: [512]u8 = undefined;
    var length: usize = 0;
    if (negative) {
        text[length] = '-';
        length += 1;
    }
    if (integer_digits == 0) {
        text[length] = '0';
        length += 1;
    } else {
        @memcpy(text[length .. length + integer_digits], digits[0..integer_digits]);
        length += integer_digits;
    }
    text[length] = '.';
    length += 1;
    if (integer_digits < digits.len) {
        text[length] = digits[integer_digits];
    } else {
        text[length] = '0';
    }
    length += 1;

    const padding = if (length >= 5) 0 else 5 - length;
    if (padding + length > buffer.len) return error.NoSpaceLeft;
    @memset(buffer[0..padding], ' ');
    @memcpy(buffer[padding .. padding + length], text[0..length]);
    return buffer[0 .. padding + length];
}

/// `str(int)` for a count column. Zig's integer formatter prints an explicit
/// "+" for a 128-bit signed value, which CPython's `%d` never does, so the
/// digits are rendered here and padded as text.
pub fn formatCount(buffer: []u8, value: i128) ![]const u8 {
    return std.fmt.bufPrint(buffer, "{s}{d}", .{
        if (value < 0) "-" else "",
        @as(u128, @intCast(if (value < 0) -value else value)),
    });
}

pub fn renderMissingReport(writer: anytype, json_path: []const u8) !void {
    try writer.print(
        "{s}: ERROR -- {s} not found; run `bash scripts/report/mcdc_report.sh` first.\n",
        .{ tool_name, json_path },
    );
}

pub fn renderUnreadableReport(writer: anytype, reason: []const u8) !void {
    try writer.print("{s}: ERROR -- cannot read MC/DC JSON: {s}\n", .{ tool_name, reason });
}

pub fn renderNoFiles(writer: anytype) !void {
    try writer.print("{s}: ERROR -- MC/DC JSON has no files.\n", .{tool_name});
}

pub fn renderMissingScopes(writer: anytype, missing: []const []const u8) !void {
    try writer.print(
        "{s}: ERROR -- no reachable decisions matched required scope(s): ",
        .{tool_name},
    );
    for (missing, 0..) |prefix, index| {
        if (index != 0) try writer.writeAll(", ");
        try writer.writeAll(prefix);
    }
    try writer.writeAll("; check the JSON path / scope.\n");
}

pub fn renderOffenders(writer: anytype, offenders: []const Offender) !void {
    try writer.print(
        "{s}: {d} first-party file(s) below the {d:.0}% reachable-MC/DC floor (NO allowlist):\n",
        .{ tool_name, offenders.len, floor_pct },
    );
    try writer.writeAll("  mc/dc  covered/reachable  file\n");
    var pct_buffer: [512]u8 = undefined;
    var covered_buffer: [64]u8 = undefined;
    var total_buffer: [64]u8 = undefined;
    for (offenders) |offender| {
        const pct = try formatPct(&pct_buffer, offender.pct);
        const covered = try formatCount(&covered_buffer, offender.covered);
        const reachable_total = try formatCount(&total_buffer, offender.reachable_total);
        try writer.print("  {s}%  {s:>5}/{s:<5}       {s}\n", .{
            pct,
            covered,
            reachable_total,
            offender.rel,
        });
    }
    try writer.writeAll(
        "Fix each at the root -- add the missing MC/DC vector (N+1 vectors " ++
            "for N conditions; see docs/MCDC.md), or, if the gap is genuinely " ++
            "unreachable on any public-API path, catalogue it with a " ++
            "`// mcdc-deactivated:` rationale per DO-178C 6.4.4.3. Do NOT add " ++
            "an allowlist.\n",
    );
}

pub fn renderPass(writer: anytype, checked: usize) !void {
    try writer.print(
        "{s}: PASS -- all {d} first-party file(s) with a reachable decision are >= {d:.0}% MC/DC.\n",
        .{ tool_name, checked, floor_pct },
    );
}

pub fn renderSelftestFailure(writer: anytype, failure: []const u8) !void {
    try writer.print("{s} selftest: FAIL -- {s}\n", .{ tool_name, failure });
}

pub fn renderSelftestPass(writer: anytype) !void {
    try writer.print(
        "{s} selftest: PASS -- scope and non-vacuity checks hold.\n",
        .{tool_name},
    );
}

/// One scope case from the predecessor's embedded self-test, in its order.
pub const ScopeCase = struct { path: []const u8, expected: bool };

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

/// The five covered production fixtures, one per required root.
const covered_fixture_paths = [_][]const u8{
    "libs/ra8_core/src/core.c",
    "apps/shared_libs/book/src/book.c",
    "examples/ek_ra8d2/demo/src/main.c",
    "port/posix/src/io.c",
    "tools/ra8_emulator/src/main.c",
};

fn entryValue(
    allocator: std.mem.Allocator,
    path: []const u8,
    covered: i64,
    total: i64,
) !std.json.Value {
    var object = std.json.ObjectMap.init(allocator);
    try object.put("file", .{ .string = path });
    try object.put("covered_decisions", .{ .integer = covered });
    try object.put("total_decisions", .{ .integer = total });
    return .{ .object = object };
}

/// The predecessor's embedded self-test, failure messages included verbatim.
///
/// It proves four things about the gate itself rather than about the tree: the
/// scope table classifies every production root and every exemption the way it
/// is documented to, a covered fixture set populates every required root, a
/// below-floor production file becomes an offender, and a report missing a
/// production root fails its non-vacuity check instead of passing.
pub fn selftestFailures(
    allocator: std.mem.Allocator,
    root_name: []const u8,
) !std.ArrayList([]const u8) {
    var failures = std.ArrayList([]const u8).init(allocator);
    errdefer failures.deinit();

    for (scope_cases) |case| {
        if (inScope(case.path) != case.expected) {
            try failures.append(try std.fmt.allocPrint(
                allocator,
                "scope mismatch for {s}: expected {s}",
                .{ case.path, if (case.expected) "True" else "False" },
            ));
        }
    }

    var covered = std.ArrayList(std.json.Value).init(allocator);
    for (covered_fixture_paths) |path| {
        try covered.append(try entryValue(allocator, path, 1, 1));
    }

    const all_covered = try collectOffenders(allocator, covered.items, root_name);
    if (all_covered.offenders.len != 0 or !std.mem.allEqual(usize, &all_covered.census, 1)) {
        try failures.append("covered fixtures did not populate every required production root");
    }

    var below_floor = std.ArrayList(std.json.Value).init(allocator);
    for (covered_fixture_paths) |path| {
        const is_port = std.mem.startsWith(u8, path, "port/");
        try below_floor.append(try entryValue(allocator, path, if (is_port) 0 else 1, 1));
    }
    const below = try collectOffenders(allocator, below_floor.items, root_name);
    if (below.offenders.len != 1 or !std.mem.eql(u8, below.offenders[0].rel, "port/posix/src/io.c")) {
        try failures.append("below-floor production fixture did not become an offender");
    }

    var missing_tools = std.ArrayList(std.json.Value).init(allocator);
    for (covered_fixture_paths) |path| {
        if (std.mem.startsWith(u8, path, "tools/")) continue;
        try missing_tools.append(try entryValue(allocator, path, 1, 1));
    }
    const without_tools = try collectOffenders(allocator, missing_tools.items, root_name);
    const missing_one = try missingScopes(allocator, without_tools.census);
    if (without_tools.offenders.len != 0 or
        missing_one.len != 1 or
        !std.mem.eql(u8, missing_one[0], "tools/"))
    {
        try failures.append("a missing production root did not fail its non-vacuity check");
    }

    var vendor_only = std.ArrayList(std.json.Value).init(allocator);
    try vendor_only.append(try entryValue(allocator, "libs/third_party/soup.c", 0, 1));
    try vendor_only.append(try entryValue(allocator, "apps/shared_libs/third_party/soup.c", 0, 1));
    const vendored = try collectOffenders(allocator, vendor_only.items, root_name);
    const missing_all = try missingScopes(allocator, vendored.census);
    if (vendored.offenders.len != 0 or missing_all.len != in_scope_prefixes.len) {
        try failures.append("exempt-only input did not fail every production non-vacuity check");
    }

    return failures;
}
