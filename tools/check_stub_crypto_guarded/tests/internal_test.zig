// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
// Behavioural regression tests for the stub-crypto guard detector (#858).
//
// Every case here pins a decision the predecessor made, so a later tidy-up
// cannot quietly widen or narrow what this gate fails on. The expectations
// were read off CPython's behaviour before the Python was deleted, not
// guessed from the pattern text.

const std = @import("std");
const implementation = @import("implementation");

fn lines(allocator: std.mem.Allocator, text: []const u8) ![][]const u8 {
    const folded = try implementation.normalizeTerminators(allocator, text);
    return implementation.splitLines(allocator, folded);
}

const guard_open = "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)";

test "the guard opener is recognised in the canonical spelling" {
    try std.testing.expect(implementation.isGuardOpen(guard_open));
}

test "the guard opener accepts either flag order" {
    try std.testing.expect(implementation.isGuardOpen(
        "#if defined(RA8_OFF_TARGET) || defined(RA8_INSECURE_STUB_CRYPTO)",
    ));
}

test "the guard opener accepts leading whitespace and a spaced hash" {
    try std.testing.expect(implementation.isGuardOpen(
        "   #  if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)",
    ));
}

test "an ifdef is not the guard opener even carrying both flags" {
    try std.testing.expect(!implementation.isGuardOpen(
        "#ifdef RA8_INSECURE_STUB_CRYPTO // or RA8_OFF_TARGET",
    ));
}

test "the guard opener needs both flags" {
    try std.testing.expect(!implementation.isGuardOpen("#if defined(RA8_OFF_TARGET)"));
    try std.testing.expect(!implementation.isGuardOpen("#if defined(RA8_INSECURE_STUB_CRYPTO)"));
}

test "a comment naming both flags is not the guard opener" {
    try std.testing.expect(!implementation.isGuardOpen(
        "// RA8_INSECURE_STUB_CRYPTO and RA8_OFF_TARGET guard the stub",
    ));
}

test "if ifdef and ifndef all open a conditional" {
    try std.testing.expect(implementation.isIf("#if FOO"));
    try std.testing.expect(implementation.isIf("#ifdef FOO"));
    try std.testing.expect(implementation.isIf("#ifndef FOO"));
}

test "an ifdefx is not a conditional opener" {
    try std.testing.expect(!implementation.isIf("#ifdefx FOO"));
    try std.testing.expect(!implementation.isIf("#ifx"));
}

test "elif is neither an opener nor an else" {
    try std.testing.expect(!implementation.isIf("#elif FOO"));
    try std.testing.expect(!implementation.isElse("#elif FOO"));
}

test "else and endif are recognised with spacing" {
    try std.testing.expect(implementation.isElse("  #  else  // fail closed"));
    try std.testing.expect(implementation.isEndif("\t#endif"));
}

test "a non-ASCII letter after endif means the line is not a directive" {
    try std.testing.expect(!implementation.isEndif("#endif\u{e9}"));
    try std.testing.expect(!implementation.isElse("#else\u{e9}"));
}

test "a non-word character after endif keeps it a directive" {
    try std.testing.expect(implementation.isEndif("#endif /* guard */"));
}

test "error is recognised as the compile-time refusal" {
    try std.testing.expect(implementation.isError("  # error stub crypto is not shippable"));
    try std.testing.expect(!implementation.isError("#errorx"));
}

test "a unicode space satisfies the whitespace before the hash" {
    try std.testing.expect(implementation.isEndif("\u{3000}#endif"));
}

test "the guard region resolves its three indices" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nbody;\n#else\n#error no\n#endif\n";
    const split = try lines(talloc, text);
    const region = implementation.findGuardRegion(split).?;
    try std.testing.expectEqual(@as(usize, 0), region.if_idx);
    try std.testing.expectEqual(@as(usize, 2), region.else_idx);
    try std.testing.expectEqual(@as(usize, 4), region.endif_idx);
}

test "a guard with no else does not resolve" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nbody;\n#endif\n";
    const split = try lines(talloc, text);
    try std.testing.expect(implementation.findGuardRegion(split) == null);
}

test "a guard with no endif does not resolve" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nbody;\n#else\n#error no\n";
    const split = try lines(talloc, text);
    try std.testing.expect(implementation.findGuardRegion(split) == null);
}

test "a nested conditional does not steal the else" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\n#ifdef INNER\nbody;\n#else\ninner;\n#endif\n#else\n#error no\n#endif\n";
    const split = try lines(talloc, text);
    const region = implementation.findGuardRegion(split).?;
    try std.testing.expectEqual(@as(usize, 6), region.else_idx);
    try std.testing.expectEqual(@as(usize, 8), region.endif_idx);
}

test "the last else at depth one wins" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nbody;\n#else\nfirst;\n#else\n#error no\n#endif\n";
    const split = try lines(talloc, text);
    const region = implementation.findGuardRegion(split).?;
    try std.testing.expectEqual(@as(usize, 4), region.else_idx);
}

test "an elif inside the guard changes nothing" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nbody;\n#elif defined(OTHER)\nalt;\n#else\n#error no\n#endif\n";
    const split = try lines(talloc, text);
    const region = implementation.findGuardRegion(split).?;
    try std.testing.expectEqual(@as(usize, 4), region.else_idx);
    try std.testing.expectEqual(@as(usize, 6), region.endif_idx);
}

test "the first guard opener in the file is the one located" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = "lead;\n" ++ guard_open ++ "\nbody;\n#else\n#error no\n#endif\n" ++ guard_open ++ "\n";
    const split = try lines(talloc, text);
    const region = implementation.findGuardRegion(split).?;
    try std.testing.expectEqual(@as(usize, 1), region.if_idx);
}

test "an else branch with a hard error return is fail-closed" {
    const body = [_][]const u8{"    return k_ra8_err_unsupported;"};
    try std.testing.expect(implementation.isFailClosed(&body));
}

test "an else branch with a compile-time error is fail-closed" {
    const body = [_][]const u8{"#error build a real crypto backend"};
    try std.testing.expect(implementation.isFailClosed(&body));
}

test "an else branch returning ok is not fail-closed" {
    const body = [_][]const u8{"    return k_ra8_ok;"};
    try std.testing.expect(!implementation.isFailClosed(&body));
}

test "an empty else branch is not fail-closed" {
    const body = [_][]const u8{};
    try std.testing.expect(!implementation.isFailClosed(&body));
}

test "a complete stub TU produces no findings" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nstatic int tok;\n#else\nreturn k_ra8_err_unsupported;\n#endif\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 0), problems.len);
}

test "a missing guard is reported once, naming the guard spelling" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = "static int tok;\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 1), problems.len);
    try std.testing.expectEqualStrings(
        "libs/x/src/y.c: missing the stub-crypto guard '" ++ guard_open ++
            "' with a matching #else / #endif",
        problems[0],
    );
}

test "a non-failing else branch is reported verbatim" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nstatic int tok;\n#else\nreturn k_ra8_ok;\n#endif\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 1), problems.len);
    try std.testing.expectEqualStrings(
        "libs/x/src/y.c: the #else branch is not fail-closed " ++
            "(needs a #error or a k_ra8_err_* hard return, not k_ra8_ok)",
        problems[0],
    );
}

test "an absent insecure body is reported" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nstatic int other;\n#else\n#error no\n#endif\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 1), problems.len);
    try std.testing.expectEqualStrings(
        "libs/x/src/y.c: insecure signature 'tok' not found inside the guarded " ++
            "#if region (is the insecure body still present and guarded?)",
        problems[0],
    );
}

test "an escaped insecure body names every escaping line" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = "static int tok;\n" ++ guard_open ++
        "\nstatic int tok;\n#else\n#error no\n#endif\nstatic int tok;\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 1), problems.len);
    try std.testing.expectEqualStrings(
        "libs/x/src/y.c: insecure signature 'tok' appears OUTSIDE the guard " ++
            "(line 1, line 7) -- the insecure body must be fully inside the " ++
            guard_open ++ " block",
        problems[0],
    );
}

test "a hit on the guard opener line itself counts as escaped" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET) // tok\n" ++
        "static int tok;\n#else\n#error no\n#endif\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 1), problems.len);
    try std.testing.expect(std.mem.indexOf(u8, problems[0], "(line 1)") != null);
}

test "a hit on the else line itself counts as escaped" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nstatic int tok;\n#else // tok\n#error no\n#endif\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 1), problems.len);
    try std.testing.expect(std.mem.indexOf(u8, problems[0], "(line 3)") != null);
}

test "the fail-closed complaint precedes the escape complaint" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nstatic int tok;\n#else\nreturn k_ra8_ok;\n#endif\nstatic int tok;\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 2), problems.len);
    try std.testing.expect(std.mem.indexOf(u8, problems[0], "not fail-closed") != null);
    try std.testing.expect(std.mem.indexOf(u8, problems[1], "OUTSIDE") != null);
}

test "a token inside the else branch escapes rather than counting" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nbody;\n#else\nreturn k_ra8_err_x; // tok\n#endif\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 2), problems.len);
    try std.testing.expect(std.mem.indexOf(u8, problems[0], "not found inside") != null);
    try std.testing.expect(std.mem.indexOf(u8, problems[1], "(line 4)") != null);
}

test "CRLF sources report the same line numbers as LF ones" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\r\nstatic int tok;\r\n#else\r\n#error no\r\n#endif\r\nstatic int tok;\r\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 1), problems.len);
    try std.testing.expect(std.mem.indexOf(u8, problems[0], "(line 6)") != null);
}

test "a form feed counts as a line break, as str.splitlines has it" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nstatic int tok;\n#else\n#error no\n#endif\na\x0cb tok\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 1), problems.len);
    try std.testing.expect(std.mem.indexOf(u8, problems[0], "(line 7)") != null);
}

test "a line separator counts as a line break too" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const text = guard_open ++ "\nstatic int tok;\n#else\n#error no\n#endif\na\u{2028}tok\n";
    const problems = try implementation.checkText(talloc, "libs/x/src/y.c", "tok", text);
    try std.testing.expectEqual(@as(usize, 1), problems.len);
    try std.testing.expect(std.mem.indexOf(u8, problems[0], "(line 7)") != null);
}

test "a unit separator is whitespace but not a line break" {
    try std.testing.expect(implementation.isPythonSpace(0x1f));
    try std.testing.expect(!implementation.isLineBreak(0x1f));
}

test "a lone carriage return folds into one line break" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const folded = try implementation.normalizeTerminators(talloc, "a\rb\r\nc\n");
    try std.testing.expectEqualStrings("a\nb\nc\n", folded);
}

test "splitlines drops no trailing content and adds no empty line" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const split = try implementation.splitLines(talloc, "a\nb\n");
    try std.testing.expectEqual(@as(usize, 2), split.len);
    try std.testing.expectEqualStrings("b", split[1]);
}

test "the missing-file finding reads as the predecessor wrote it" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const rendered = try implementation.renderMissingFile(talloc, "libs/x/src/y.c");
    try std.testing.expectEqualStrings(
        "libs/x/src/y.c: file not found (expected an insecure stub TU here)",
        rendered,
    );
}

test "the governed set holds the eight stub TUs with distinct tokens" {
    try std.testing.expectEqual(@as(usize, 8), implementation.stub_tus.len);
    for (implementation.stub_tus, 0..) |left, index| {
        for (implementation.stub_tus[index + 1 ..]) |right| {
            try std.testing.expect(!std.mem.eql(u8, left.rel, right.rel));
            try std.testing.expect(!std.mem.eql(u8, left.token, right.token));
        }
    }
}

test "every governed TU sits under libs and names a C source" {
    for (implementation.stub_tus) |stub| {
        try std.testing.expect(std.mem.startsWith(u8, stub.rel, "libs/"));
        try std.testing.expect(std.mem.endsWith(u8, stub.rel, ".c"));
    }
}

test "both selftest directions hold" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const cases = try implementation.selftestCases(talloc);
    for (cases) |case| try std.testing.expect(case.passed);
}

test "the selftest good fixture is genuinely clean" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const problems = try implementation.checkText(
        talloc,
        implementation.selftest_rel,
        implementation.selftest_signature,
        implementation.selftest_good,
    );
    try std.testing.expectEqual(@as(usize, 0), problems.len);
}

test "the selftest bad fixture fires on both counts" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const talloc = arena.allocator();
    const problems = try implementation.checkText(
        talloc,
        implementation.selftest_rel,
        implementation.selftest_signature,
        implementation.selftest_bad,
    );
    try std.testing.expect(problems.len >= implementation.minimum_bad_findings);
}

test "the guard spelling is assembled from the two flags" {
    try std.testing.expect(std.mem.indexOf(u8, implementation.guard_spelling, implementation.insecure_flag) != null);
    try std.testing.expect(std.mem.indexOf(u8, implementation.guard_spelling, implementation.off_target_flag) != null);
}

test "charAt decodes a multi-byte code point and a stray byte alike" {
    const decoded = implementation.charAt("\u{e9}x", 0).?;
    try std.testing.expectEqual(@as(u21, 0xe9), decoded.code_point);
    try std.testing.expectEqual(@as(usize, 2), decoded.len);
    const stray = implementation.charAt("\xff", 0).?;
    try std.testing.expectEqual(@as(usize, 1), stray.len);
}
