//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ci-monitor status reader, as pure computation (#858, #1144).
//!
//! Everything here is a function of an already-parsed JSON document: no file
//! system, no argv, no process state, so every rule below is provable with no
//! status file on disk. The argv membrane and the exit contract live in
//! `../cli.zig`.
//!
//! The rules are inherited from the Python this replaced, not reinvented. Two
//! of them are load-bearing and were each paid for with a wrong verdict in
//! anger:
//!
//! * SKIPPED IS NOT SUCCESS -- an all-skipped sha ran no gate, so it is
//!   UNKNOWN, never PASS (#530).
//! * CANCELLED IS NOT FAILURE -- a superseded run is a non-result, not a red;
//!   a workflow is judged by its latest run that actually concluded (#561).
//!
//! The Python rendered values with `str()` and tested truth with Python's
//! truthiness, and `monitor.sh` reads those exact bytes, so both are
//! reproduced here rather than approximated.

const std = @import("std");

pub const Value = std.json.Value;

/// Conclusions that make a run decisively red (the Python's `FAIL_CONC`).
pub const fail_conclusions = [_][]const u8{ "failure", "timed_out" };

/// Rows `lines-head` prints, matching the Python's `HEAD_ROWS`.
pub const head_rows: usize = 6;

/// Code points of a sha kept in a `lines-head` row (`SHA_ABBREV`).
pub const sha_abbrev: usize = 9;

/// Errors a value render can raise. Spelled out rather than inferred because
/// `appendRepr` and `pythonStr` are mutually recursive through a container's
/// members, and an inferred set cannot close that cycle.
pub const RenderError = std.mem.Allocator.Error || error{ NoSpaceLeft, Overflow, InvalidCharacter };

/// Document shapes on which the Python raised instead of answering. Each one
/// maps to its exit status in `cli.run`; none of them is a silent pass.
pub const ShapeError = error{
    /// The top-level value is not an object, so `doc.get` had no meaning.
    DocumentNotAnObject,
    /// `runs` is truthy but not a list: Python iterated it and `r.get` raised.
    RunsNotAList,
    /// A run entry is not an object, so `r.get` raised.
    RunNotAnObject,
    /// A run's `name` is a list or dict: an unhashable grouping key.
    NameNotHashable,
};

/// The object behind `value`, or null when it is not an object.
pub fn objectOf(value: Value) ?std.json.ObjectMap {
    return switch (value) {
        .object => |map| map,
        else => null,
    };
}

/// `value.get(key)` for an object, null for anything else or a missing key.
pub fn get(value: Value, key: []const u8) ?Value {
    const map = objectOf(value) orelse return null;
    return map.get(key);
}

fn isZeroLiteral(text: []const u8) bool {
    for (text) |byte| {
        if (byte >= '1' and byte <= '9') return false;
    }
    return true;
}

/// Python truthiness, which is what `x or ""` and `or []` turn on: null,
/// false, zero, an empty string and an empty container are all falsy.
pub fn isTruthy(value: Value) bool {
    return switch (value) {
        .null => false,
        .bool => |flag| flag,
        .integer => |number| number != 0,
        .float => |number| number != 0.0,
        .number_string => |text| !isZeroLiteral(text),
        .string => |text| text.len > 0,
        .array => |items| items.items.len > 0,
        .object => |map| map.count() > 0,
    };
}

/// `repr()` of a float, in Python's form: shortest round-trip digits, fixed
/// notation while the decimal exponent is in -4..15 and always carrying a
/// fractional digit there, scientific outside it with a signed, two-digit
/// exponent (`1e+30`, `1e-05`).
pub fn pythonFloat(allocator: std.mem.Allocator, number: f64) RenderError![]const u8 {
    if (std.math.isNan(number)) return allocator.dupe(u8, "nan");
    if (std.math.isInf(number)) return allocator.dupe(u8, if (number < 0) "-inf" else "inf");

    var buffer: [512]u8 = undefined;
    const scientific = try std.fmt.bufPrint(&buffer, "{e}", .{number});

    const split = std.mem.indexOfScalar(u8, scientific, 'e') orelse
        return allocator.dupe(u8, scientific);
    const mantissa = scientific[0..split];
    const exponent = try std.fmt.parseInt(i32, scientific[split + 1 ..], 10);

    const negative = mantissa.len > 0 and mantissa[0] == '-';
    const body = if (negative) mantissa[1..] else mantissa;
    var digits_buffer: [64]u8 = undefined;
    var digits_len: usize = 0;
    for (body) |byte| {
        if (byte == '.') continue;
        digits_buffer[digits_len] = byte;
        digits_len += 1;
    }
    const digits = digits_buffer[0..digits_len];
    const sign: []const u8 = if (negative) "-" else "";

    if (exponent >= -4 and exponent <= 15) {
        var text = std.ArrayList(u8).init(allocator);
        errdefer text.deinit();
        try text.appendSlice(sign);
        if (exponent < 0) {
            try text.appendSlice("0.");
            var zeros = -exponent - 1;
            while (zeros > 0) : (zeros -= 1) try text.append('0');
            try text.appendSlice(digits);
        } else {
            const point: usize = @intCast(exponent + 1);
            if (digits.len > point) {
                try text.appendSlice(digits[0..point]);
                try text.append('.');
                try text.appendSlice(digits[point..]);
            } else {
                try text.appendSlice(digits);
                var pad = point - digits.len;
                while (pad > 0) : (pad -= 1) try text.append('0');
                try text.appendSlice(".0");
            }
        }
        return text.toOwnedSlice();
    }

    const tail = if (digits.len > 1) digits[1..] else "";
    const point: []const u8 = if (digits.len > 1) "." else "";
    const exponent_sign: u8 = if (exponent < 0) '-' else '+';
    const magnitude: u32 = @intCast(if (exponent < 0) -exponent else exponent);
    return std.fmt.allocPrint(allocator, "{s}{c}{s}{s}e{c}{d:0>2}", .{
        sign,
        digits[0],
        point,
        tail,
        exponent_sign,
        magnitude,
    });
}

/// `repr()` of a string: single quotes unless the text holds one and no
/// double quote, with the escapes Python spells and printable code points
/// passed through untouched.
pub fn appendStringRepr(text: *std.ArrayList(u8), value: []const u8) RenderError!void {
    const has_single = std.mem.indexOfScalar(u8, value, '\'') != null;
    const has_double = std.mem.indexOfScalar(u8, value, '"') != null;
    const quote: u8 = if (has_single and !has_double) '"' else '\'';
    try text.append(quote);
    for (value) |byte| {
        switch (byte) {
            '\\' => try text.appendSlice("\\\\"),
            '\n' => try text.appendSlice("\\n"),
            '\r' => try text.appendSlice("\\r"),
            '\t' => try text.appendSlice("\\t"),
            else => {
                if (byte == quote) {
                    try text.append('\\');
                    try text.append(byte);
                } else if (byte < 0x20 or byte == 0x7f) {
                    try text.writer().print("\\x{x:0>2}", .{byte});
                } else {
                    try text.append(byte);
                }
            },
        }
    }
    try text.append(quote);
}

/// `repr()` of any value: the same as `str()` for every scalar except a
/// string, which gains its quotes, and this is what a container's members
/// render with.
pub fn appendRepr(text: *std.ArrayList(u8), value: Value) RenderError!void {
    switch (value) {
        .string => |inner| try appendStringRepr(text, inner),
        .array => |items| {
            try text.append('[');
            for (items.items, 0..) |item, index| {
                if (index > 0) try text.appendSlice(", ");
                try appendRepr(text, item);
            }
            try text.append(']');
        },
        .object => |map| {
            try text.append('{');
            var index: usize = 0;
            var entries = map.iterator();
            while (entries.next()) |entry| : (index += 1) {
                if (index > 0) try text.appendSlice(", ");
                try appendStringRepr(text, entry.key_ptr.*);
                try text.appendSlice(": ");
                try appendRepr(text, entry.value_ptr.*);
            }
            try text.append('}');
        },
        else => {
            const scalar = try pythonStr(text.allocator, value);
            defer text.allocator.free(scalar);
            try text.appendSlice(scalar);
        },
    }
}

/// `str(value)` for every value kind a status document can carry, including
/// the containers monitor.sh never asks for: a list or dict renders through
/// `repr` like Python's `str` of a container does, quotes and all.
pub fn pythonStr(allocator: std.mem.Allocator, value: Value) RenderError![]const u8 {
    return switch (value) {
        .null => allocator.dupe(u8, "None"),
        .bool => |flag| allocator.dupe(u8, if (flag) "True" else "False"),
        .integer => |number| std.fmt.allocPrint(allocator, "{d}", .{number}),
        .float => |number| pythonFloat(allocator, number),
        .number_string => |text| allocator.dupe(u8, text),
        .string => |text| allocator.dupe(u8, text),
        .array, .object => blk: {
            var text = std.ArrayList(u8).init(allocator);
            errdefer text.deinit();
            try appendRepr(&text, value);
            break :blk text.toOwnedSlice();
        },
    };
}

/// `str(doc.get(key) or "")`: the empty string for a missing or falsy field.
pub fn fieldText(allocator: std.mem.Allocator, value: Value, key: []const u8) RenderError![]const u8 {
    const raw = get(value, key) orelse return allocator.dupe(u8, "");
    if (!isTruthy(raw)) return allocator.dupe(u8, "");
    return pythonStr(allocator, raw);
}

/// `doc.get("runs") or []`, with the shapes Python raised on kept as errors.
pub fn runsOf(value: Value) ShapeError![]const Value {
    const map = objectOf(value) orelse return error.DocumentNotAnObject;
    const raw = map.get("runs") orelse return &.{};
    if (!isTruthy(raw)) return &.{};
    return switch (raw) {
        .array => |items| items.items,
        else => error.RunsNotAList,
    };
}

/// Reject a run list holding a non-object entry, where `r.get` raised.
pub fn requireRunObjects(runs: []const Value) ShapeError!void {
    for (runs) |run| {
        if (objectOf(run) == null) return error.RunNotAnObject;
    }
}

/// True when `run`'s conclusion is exactly the string `conclusion`.
pub fn conclusionIs(run: Value, conclusion: []const u8) bool {
    const raw = get(run, "conclusion") orelse return false;
    return switch (raw) {
        .string => |text| std.mem.eql(u8, text, conclusion),
        else => false,
    };
}

/// True when the conclusion is one of the decisively red ones.
pub fn isFailConclusion(run: Value) bool {
    for (fail_conclusions) |conclusion| {
        if (conclusionIs(run, conclusion)) return true;
    }
    return false;
}

/// True when the run actually concluded success or failure. Only such a run
/// can decide its workflow; cancelled and skipped are non-results.
pub fn isDecisive(run: Value) bool {
    return conclusionIs(run, "success") or isFailConclusion(run);
}

/// True when `run.get("status") != "completed"`, so a non-string status counts
/// as still in flight exactly as it did in Python.
pub fn isInFlight(run: Value) bool {
    const raw = get(run, "status") orelse return true;
    return switch (raw) {
        .string => |text| !std.mem.eql(u8, text, "completed"),
        else => true,
    };
}

/// `str(run.get(key) or "")` for one run field.
pub fn runText(allocator: std.mem.Allocator, run: Value, key: []const u8) ![]const u8 {
    return fieldText(allocator, run, key);
}

/// True when the run's sha starts with `prefix`; the empty prefix matches any
/// run, including one with no sha at all.
pub fn matchesSha(allocator: std.mem.Allocator, run: Value, prefix: []const u8) !bool {
    const sha = try runText(allocator, run, "sha");
    defer allocator.free(sha);
    return std.mem.startsWith(u8, sha, prefix);
}

/// The runs whose sha starts with `prefix`, in document order.
pub fn matching(allocator: std.mem.Allocator, runs: []const Value, prefix: []const u8) ![]Value {
    var picked = std.ArrayList(Value).init(allocator);
    errdefer picked.deinit();
    for (runs) |run| {
        if (try matchesSha(allocator, run, prefix)) try picked.append(run);
    }
    return picked.toOwnedSlice();
}

/// Count of the sha's runs whose conclusion equals `conclusion`.
pub fn conclusionCount(
    allocator: std.mem.Allocator,
    runs: []const Value,
    prefix: []const u8,
    conclusion: []const u8,
) !usize {
    var total: usize = 0;
    for (runs) |run| {
        if (!try matchesSha(allocator, run, prefix)) continue;
        if (conclusionIs(run, conclusion)) total += 1;
    }
    return total;
}

/// The first `count` code points of `text`, so a sha abbreviation cuts where
/// Python's slice cut rather than mid-sequence.
pub fn truncateCodePoints(text: []const u8, count: usize) []const u8 {
    var index: usize = 0;
    var seen: usize = 0;
    while (index < text.len and seen < count) : (seen += 1) {
        const length = std.unicode.utf8ByteSequenceLength(text[index]) catch 1;
        index = @min(text.len, index + length);
    }
    return text[0..index];
}

/// One run as monitor.sh prints it: `  <name>: <status>/<conclusion>`, with a
/// missing name or status rendering as `None` and a falsy conclusion as `-`.
pub fn renderRun(allocator: std.mem.Allocator, run: Value, with_sha: bool) ![]const u8 {
    const name = try pythonStr(allocator, get(run, "name") orelse .null);
    defer allocator.free(name);
    const status = try pythonStr(allocator, get(run, "status") orelse .null);
    defer allocator.free(status);

    const raw_conclusion = get(run, "conclusion") orelse Value{ .null = {} };
    const conclusion = if (isTruthy(raw_conclusion))
        try pythonStr(allocator, raw_conclusion)
    else
        try allocator.dupe(u8, "-");
    defer allocator.free(conclusion);

    if (!with_sha) {
        return std.fmt.allocPrint(allocator, "  {s}: {s}/{s}", .{ name, status, conclusion });
    }
    const sha = try runText(allocator, run, "sha");
    defer allocator.free(sha);
    return std.fmt.allocPrint(allocator, "  {s}: {s}/{s}  {s}", .{
        name,
        status,
        conclusion,
        truncateCodePoints(sha, sha_abbrev),
    });
}

/// One workflow's verdict for a sha.
pub const WorkflowVerdict = enum { pass, fail, running, noresult };

fn lessByCreated(context: []const []const u8, left: usize, right: usize) bool {
    return std.mem.order(u8, context[left], context[right]) == .lt;
}

/// The verdict for one workflow's runs of a sha.
///
/// The latest run that actually concluded decides it, so a re-run that
/// succeeded clears an earlier failure and a superseded `cancelled` (or a
/// `skipped`) run never overrides a real conclusion. Ordering is by the
/// `created` field as a string, sorted stably, so runs with no `created` keep
/// document order and the last one wins, exactly as Python's `sorted` left it.
pub fn workflowVerdict(allocator: std.mem.Allocator, wf_runs: []const Value) !WorkflowVerdict {
    var decisive = std.ArrayList(usize).init(allocator);
    defer decisive.deinit();
    var keys = std.ArrayList([]const u8).init(allocator);
    defer {
        for (keys.items) |key| allocator.free(key);
        keys.deinit();
    }

    for (wf_runs, 0..) |run, index| {
        if (!isDecisive(run)) continue;
        try decisive.append(index);
        try keys.append(try runText(allocator, run, "created"));
    }

    if (decisive.items.len > 0) {
        const order = try allocator.alloc(usize, decisive.items.len);
        defer allocator.free(order);
        for (order, 0..) |*slot, index| slot.* = index;
        std.sort.insertion(usize, order, @as([]const []const u8, keys.items), lessByCreated);
        const winner = wf_runs[decisive.items[order[order.len - 1]]];
        return if (conclusionIs(winner, "success")) .pass else .fail;
    }

    for (wf_runs) |run| {
        if (isInFlight(run)) return .running;
    }
    return .noresult;
}

/// The grouping key for a run's workflow name.
///
/// Python grouped on the raw value, so numeric equality collapses (`True`,
/// `1` and `1.0` are one dict key) while a string never collides with a
/// number, and an unhashable list or dict raised. All three hold here.
pub fn nameKey(allocator: std.mem.Allocator, run: Value) ![]const u8 {
    const raw = get(run, "name") orelse Value{ .null = {} };
    return switch (raw) {
        .null => allocator.dupe(u8, "none:"),
        .bool => |flag| std.fmt.allocPrint(allocator, "num:{d}", .{@as(u8, if (flag) 1 else 0)}),
        .integer => |number| std.fmt.allocPrint(allocator, "num:{d}", .{number}),
        .float => |number| blk: {
            if (number == @trunc(number) and std.math.isFinite(number) and
                @abs(number) < 9.007199254740992e15)
            {
                break :blk std.fmt.allocPrint(allocator, "num:{d}", .{@as(i64, @intFromFloat(number))});
            }
            const text = try pythonFloat(allocator, number);
            defer allocator.free(text);
            break :blk std.fmt.allocPrint(allocator, "num:{s}", .{text});
        },
        .number_string => |text| std.fmt.allocPrint(allocator, "num:{s}", .{text}),
        .string => |text| std.fmt.allocPrint(allocator, "str:{s}", .{text}),
        .array, .object => error.NameNotHashable,
    };
}

/// A sha's aggregate verdict: `PASS`, `FAIL` or `UNKNOWN`.
///
/// No run for the sha is UNKNOWN (never a vacuous pass). A red workflow makes
/// the sha FAIL; an in-flight one leaves it UNDECIDED; and a sha whose every
/// workflow was cancelled or skipped is UNKNOWN, because nothing ran.
pub fn verdict(
    allocator: std.mem.Allocator,
    runs: []const Value,
    prefix: []const u8,
) ![]const u8 {
    const got = try matching(allocator, runs, prefix);
    defer allocator.free(got);
    if (got.len == 0) return allocator.dupe(u8, "UNKNOWN");

    var groups = std.StringArrayHashMap(std.ArrayList(Value)).init(allocator);
    defer {
        for (groups.keys()) |key| allocator.free(key);
        for (groups.values()) |*list| list.deinit();
        groups.deinit();
    }

    for (got) |run| {
        const key = try nameKey(allocator, run);
        const entry = try groups.getOrPut(key);
        if (entry.found_existing) {
            allocator.free(key);
        } else {
            entry.value_ptr.* = std.ArrayList(Value).init(allocator);
        }
        try entry.value_ptr.append(run);
    }

    var running = false;
    var passing = false;
    for (groups.values()) |group| {
        switch (try workflowVerdict(allocator, group.items)) {
            .fail => return allocator.dupe(u8, "FAIL"),
            .running => running = true,
            .pass => passing = true,
            .noresult => {},
        }
    }
    if (running) return allocator.dupe(u8, "UNKNOWN");
    if (passing) return allocator.dupe(u8, "PASS");
    // Every workflow was cancelled or skipped: nothing actually ran.
    return allocator.dupe(u8, "UNKNOWN");
}
