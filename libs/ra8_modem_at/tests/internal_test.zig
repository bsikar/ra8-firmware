//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! State-free core tests: the string utilities, the final-result-code table,
//! the capture appender, the three promoted AND-decisions, the line
//! accumulator and the URC slot table.

const std = @import("std");
const core = @import("implementation");

fn z(comptime s: []const u8) [*]const u8 {
    return @ptrCast(s.ptr);
}

test "strLen counts to the first NUL" {
    try std.testing.expectEqual(@as(u16, 0), core.strLen(z("")));
    try std.testing.expectEqual(@as(u16, 2), core.strLen(z("OK")));
    try std.testing.expectEqual(@as(u16, 5), core.strLen(z("+CSQ:")));
}

test "strLen handles an embedded NUL as the terminator" {
    const buf = [_]u8{ 'A', 'T', 0, 'X', 0 };
    try std.testing.expectEqual(@as(u16, 2), core.strLen(&buf));
}

test "startsWith matches a leading prefix" {
    try std.testing.expectEqual(@as(u8, 1), core.startsWith(z("+CSQ: 20,99"), z("+CSQ:")));
    try std.testing.expectEqual(@as(u8, 0), core.startsWith(z("+CREG: 1"), z("+CSQ:")));
}

test "startsWith treats an empty needle as a match" {
    try std.testing.expectEqual(@as(u8, 1), core.startsWith(z("anything"), z("")));
    try std.testing.expectEqual(@as(u8, 1), core.startsWith(z(""), z("")));
}

test "startsWith rejects a needle longer than the haystack" {
    try std.testing.expectEqual(@as(u8, 0), core.startsWith(z("+CS"), z("+CSQ:")));
}

test "strEq is exact including length" {
    try std.testing.expectEqual(@as(u8, 1), core.strEq(z("OK"), z("OK")));
    try std.testing.expectEqual(@as(u8, 0), core.strEq(z("OK"), z("OKAY")));
    try std.testing.expectEqual(@as(u8, 0), core.strEq(z("OKAY"), z("OK")));
    try std.testing.expectEqual(@as(u8, 0), core.strEq(z("OK"), z("NO")));
    try std.testing.expectEqual(@as(u8, 1), core.strEq(z(""), z("")));
    try std.testing.expectEqual(@as(u8, 0), core.strEq(z(""), z("OK")));
}

test "classifyFinal recognises OK as the only non-error final code" {
    var is_err: u8 = 9;
    try std.testing.expectEqual(@as(u8, 1), core.classifyFinal(z("OK"), &is_err));
    try std.testing.expectEqual(@as(u8, 0), is_err);
}

test "classifyFinal recognises every error final code" {
    const cases = [_][]const u8{ "ERROR", "BUSY", "NO CARRIER" };
    for (cases) |c| {
        var is_err: u8 = 0;
        const line = try std.testing.allocator.dupeZ(u8, c);
        defer std.testing.allocator.free(line);
        try std.testing.expectEqual(@as(u8, 1), core.classifyFinal(line.ptr, &is_err));
        try std.testing.expectEqual(@as(u8, 1), is_err);
    }
}

test "classifyFinal treats CME and CMS ERROR as prefixes" {
    var is_err: u8 = 0;
    try std.testing.expectEqual(@as(u8, 1), core.classifyFinal(z("+CME ERROR: 10"), &is_err));
    try std.testing.expectEqual(@as(u8, 1), is_err);
    is_err = 0;
    try std.testing.expectEqual(@as(u8, 1), core.classifyFinal(z("+CMS ERROR: 321"), &is_err));
    try std.testing.expectEqual(@as(u8, 1), is_err);
}

test "classifyFinal rejects payload lines and clears is_error first" {
    var is_err: u8 = 1;
    try std.testing.expectEqual(@as(u8, 0), core.classifyFinal(z("+CSQ: 20,99"), &is_err));
    try std.testing.expectEqual(@as(u8, 0), is_err);
}

test "classifyFinal does not match a longer line with an OK prefix" {
    var is_err: u8 = 0;
    try std.testing.expectEqual(@as(u8, 0), core.classifyFinal(z("OKAY"), &is_err));
}

test "appendCh keeps room for the terminator" {
    var buf = [_]u8{0} ** 4;
    var used: usize = 0;
    core.appendCh(&buf, buf.len, &used, 'a');
    core.appendCh(&buf, buf.len, &used, 'b');
    core.appendCh(&buf, buf.len, &used, 'c');
    core.appendCh(&buf, buf.len, &used, 'd');
    try std.testing.expectEqual(@as(usize, 3), used);
    try std.testing.expectEqualStrings("abc", buf[0..3]);
    try std.testing.expectEqual(@as(u8, 0), buf[3]);
}

test "appendCh writes nothing into a one-byte buffer" {
    var buf = [_]u8{0xAA};
    var used: usize = 0;
    core.appendCh(&buf, buf.len, &used, 'z');
    try std.testing.expectEqual(@as(usize, 0), used);
    try std.testing.expectEqual(@as(u8, 0xAA), buf[0]);
}

test "captureLine newline-separates successive lines" {
    var buf = [_]u8{0} ** 32;
    var used: usize = 0;
    core.captureLine(z("+CSQ: 20,99"), &buf, buf.len, &used);
    core.captureLine(z("second"), &buf, buf.len, &used);
    try std.testing.expectEqualStrings("+CSQ: 20,99\nsecond", buf[0..used]);
    try std.testing.expectEqual(@as(u8, 0), buf[used]);
}

test "captureLine truncates silently and stays NUL terminated" {
    var buf = [_]u8{0} ** 6;
    var used: usize = 0;
    core.captureLine(z("0123456789"), &buf, buf.len, &used);
    try std.testing.expectEqual(@as(usize, 5), used);
    try std.testing.expectEqualStrings("01234", buf[0..5]);
    try std.testing.expectEqual(@as(u8, 0), buf[5]);
}

test "captureLine is a no-op for a NULL buffer or a zero capacity" {
    var used: usize = 0;
    core.captureLine(z("x"), null, 8, &used);
    try std.testing.expectEqual(@as(usize, 0), used);
    var buf = [_]u8{0xAA} ** 2;
    core.captureLine(z("x"), &buf, 0, &used);
    try std.testing.expectEqual(@as(usize, 0), used);
    try std.testing.expectEqual(@as(u8, 0xAA), buf[0]);
}

test "resetLineShouldClear covers all four input combinations" {
    var byte: u8 = 0;
    try std.testing.expectEqual(@as(u8, 1), core.resetLineShouldClear(&byte, 16));
    try std.testing.expectEqual(@as(u8, 0), core.resetLineShouldClear(&byte, 0));
    try std.testing.expectEqual(@as(u8, 0), core.resetLineShouldClear(null, 16));
    try std.testing.expectEqual(@as(u8, 0), core.resetLineShouldClear(null, 0));
}

test "payloadPrefixMatches covers all four short-circuit vectors" {
    try std.testing.expectEqual(@as(u8, 1), core.payloadPrefixMatches(z("+CSQ: 20"), z("+CSQ:")));
    try std.testing.expectEqual(@as(u8, 0), core.payloadPrefixMatches(z("+CSQ: 20"), z("+CREG:")));
    try std.testing.expectEqual(@as(u8, 0), core.payloadPrefixMatches(z("+CSQ: 20"), z("")));
    try std.testing.expectEqual(@as(u8, 0), core.payloadPrefixMatches(z("+CSQ: 20"), null));
}

test "captureShouldClear covers all four input combinations" {
    var byte: u8 = 0;
    try std.testing.expectEqual(@as(u8, 1), core.captureShouldClear(&byte, 4));
    try std.testing.expectEqual(@as(u8, 0), core.captureShouldClear(&byte, 0));
    try std.testing.expectEqual(@as(u8, 0), core.captureShouldClear(null, 4));
    try std.testing.expectEqual(@as(u8, 0), core.captureShouldClear(null, 0));
}

test "effectiveTimeout prefers the request, then the config, then the default" {
    try std.testing.expectEqual(@as(u16, 250), core.effectiveTimeout(250, 2000));
    try std.testing.expectEqual(@as(u16, 2000), core.effectiveTimeout(0, 2000));
    try std.testing.expectEqual(core.default_timeout_ms, core.effectiveTimeout(0, 0));
}

test "seenExpSeed starts satisfied when no prefix was asked for" {
    try std.testing.expectEqual(@as(u8, 1), core.seenExpSeed(null));
    try std.testing.expectEqual(@as(u8, 1), core.seenExpSeed(z("")));
    try std.testing.expectEqual(@as(u8, 0), core.seenExpSeed(z("+CSQ:")));
}

test "accumulator emits a line on CR and on LF" {
    var buf = [_]u8{0} ** 32;
    var acc = core.Accumulator{ .buf = &buf, .cap = buf.len };
    try std.testing.expect(acc.push('O') == null);
    try std.testing.expect(acc.push('K') == null);
    const line = acc.push('\r') orelse return error.NoLine;
    try std.testing.expectEqualStrings("OK", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(line)), 0));
    try std.testing.expectEqual(@as(u16, 0), acc.len);
    try std.testing.expect(acc.push('A') == null);
    const second = acc.push('\n') orelse return error.NoLine;
    try std.testing.expectEqualStrings("A", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(second)), 0));
}

test "accumulator swallows a bare CRLF pair without emitting an empty line" {
    var buf = [_]u8{0} ** 16;
    var acc = core.Accumulator{ .buf = &buf, .cap = buf.len };
    try std.testing.expect(acc.push('\r') == null);
    try std.testing.expect(acc.push('\n') == null);
}

test "accumulator emits early on overflow and drops the overflowing byte" {
    var buf = [_]u8{0} ** 4;
    var acc = core.Accumulator{ .buf = &buf, .cap = buf.len };
    try std.testing.expect(acc.push('a') == null);
    try std.testing.expect(acc.push('b') == null);
    try std.testing.expect(acc.push('c') == null);
    const line = acc.push('d') orelse return error.NoLine;
    try std.testing.expectEqualStrings("abc", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(line)), 0));
    try std.testing.expectEqual(@as(u16, 0), acc.len);
}

test "accumulator reset empties the buffer and holds nothing without one" {
    var buf = [_]u8{ 'x', 'y', 0 };
    var acc = core.Accumulator{ .buf = &buf, .cap = buf.len, .len = 2 };
    acc.reset();
    try std.testing.expectEqual(@as(u16, 0), acc.len);
    try std.testing.expectEqual(@as(u8, 0), buf[0]);

    var empty = core.Accumulator{};
    empty.reset();
    try std.testing.expect(empty.push('a') == null);
}

const TestFn = *const fn (line: [*:0]const u8, ctx: ?*anyopaque) callconv(.c) void;
const TestTable = core.UrcTable(TestFn);

var seen_calls: u32 = 0;
fn handlerA(line: [*:0]const u8, ctx: ?*anyopaque) callconv(.c) void {
    _ = line;
    _ = ctx;
    seen_calls += 1;
}
fn handlerB(line: [*:0]const u8, ctx: ?*anyopaque) callconv(.c) void {
    _ = line;
    _ = ctx;
    seen_calls += 100;
}

test "table insert takes the first free slot and match finds by prefix" {
    var table = TestTable{};
    try std.testing.expectEqual(@as(u8, 1), table.insert(z("+CMTI:"), 6, handlerA, null));
    try std.testing.expectEqual(@as(u8, 1), table.insert(z("+CREG:"), 6, handlerB, null));
    try std.testing.expectEqual(@as(u8, 2), table.used());
    const slot = table.match(z("+CREG: 1,\"1A2B\"")) orelse return error.NoMatch;
    try std.testing.expectEqual(@as(?TestFn, handlerB), slot.handler);
    try std.testing.expect(table.match(z("+CSQ: 20,99")) == null);
}

test "table replace rebinds an existing prefix without consuming a slot" {
    var table = TestTable{};
    _ = table.insert(z("+CMTI:"), 6, handlerA, null);
    try std.testing.expectEqual(@as(u8, 1), table.replace(z("+CMTI:"), handlerB, null));
    try std.testing.expectEqual(@as(u8, 1), table.used());
    const slot = table.match(z("+CMTI: \"SM\",3")) orelse return error.NoMatch;
    try std.testing.expectEqual(@as(?TestFn, handlerB), slot.handler);
    try std.testing.expectEqual(@as(u8, 0), table.replace(z("+CREG:"), handlerA, null));
}

test "table fills at eight slots and clear empties it" {
    var table = TestTable{};
    var i: u8 = 0;
    while (i < core.max_unsolicited) : (i += 1) {
        const prefix = [_]u8{ '+', 'A' + i, ':', 0 };
        try std.testing.expectEqual(@as(u8, 1), table.insert(&prefix, 3, handlerA, null));
    }
    try std.testing.expectEqual(core.max_unsolicited, table.used());
    try std.testing.expectEqual(@as(u8, 0), table.insert(z("+ZZ:"), 4, handlerA, null));
    table.clear();
    try std.testing.expectEqual(@as(u8, 0), table.used());
    try std.testing.expect(table.match(z("+A:")) == null);
}

test "table match keeps registration order when two prefixes both fit" {
    var table = TestTable{};
    _ = table.insert(z("+C"), 2, handlerA, null);
    _ = table.insert(z("+CREG:"), 6, handlerB, null);
    const slot = table.match(z("+CREG: 1")) orelse return error.NoMatch;
    try std.testing.expectEqual(@as(?TestFn, handlerA), slot.handler);
}
