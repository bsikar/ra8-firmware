//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Detector and scope algebra of the pointer-only comment gate (#858). Every
//! case here is the behaviour the Python gate had, pinned so the migration
//! cannot quietly widen or narrow it.

const std = @import("std");
const implementation = @import("implementation");

/// Existence oracle over a fixed list, so scope is provable with no repository.
const ListResolver = struct {
    present: []const []const u8,

    fn resolver(self: *const ListResolver) implementation.Resolver {
        return .{ .context = self, .is_file_fn = thunk };
    }

    fn thunk(context: *const anyopaque, rel: []const u8) bool {
        const self: *const ListResolver = @ptrCast(@alignCast(context));
        for (self.present) |candidate| {
            if (std.mem.eql(u8, candidate, rel)) return true;
        }
        return false;
    }
};

test "the plain generated sentence fires" {
    try std.testing.expect(implementation.isBanned("/* see header for the documented contract. */"));
}

test "the optional the and internal words fire" {
    try std.testing.expect(implementation.isBanned("/* see the header for the documented contract. */"));
    try std.testing.expect(implementation.isBanned("/* see internal header for the documented contract. */"));
    try std.testing.expect(
        implementation.isBanned("/* see the internal header for the documented contract. */"),
    );
}

test "the match is case insensitive" {
    try std.testing.expect(implementation.isBanned("/* SEE THE INTERNAL HEADER FOR THE DOCUMENTED CONTRACT. */"));
    try std.testing.expect(implementation.isBanned("/* See Header For The Documented Contract. */"));
}

test "leading and trailing whitespace is allowed" {
    try std.testing.expect(implementation.isBanned("    /* see header for the documented contract. */"));
    try std.testing.expect(implementation.isBanned("\t/* see header for the documented contract. */   "));
}

test "whitespace inside the comment delimiters is allowed" {
    try std.testing.expect(implementation.isBanned("/*   see header for the documented contract.   */"));
}

test "a trailing note keeps the line quiet" {
    try std.testing.expect(
        !implementation.isBanned("/* See header for the documented contract -- bounded scan. */"),
    );
}

test "legacy wording stays quiet" {
    try std.testing.expect(!implementation.isBanned("/* see header for full description */"));
}

test "a string literal stays quiet" {
    try std.testing.expect(
        !implementation.isBanned("const char* text = \"see header for the documented contract.\";"),
    );
}

test "a missing full stop stays quiet" {
    try std.testing.expect(!implementation.isBanned("/* see header for the documented contract */"));
}

test "double spacing between words stays quiet" {
    // The spaces between the words are literal single spaces in the rule, not
    // `\s`, so a re-wrapped sentence is not the generated form.
    try std.testing.expect(!implementation.isBanned("/* see  header for the documented contract. */"));
}

test "a line comment form stays quiet" {
    try std.testing.expect(!implementation.isBanned("// see header for the documented contract."));
}

test "trailing code after the comment stays quiet" {
    try std.testing.expect(
        !implementation.isBanned("/* see header for the documented contract. */ int x = 1;"),
    );
}

test "the internal word only counts after the optional the" {
    // `(?:the )?(?:internal )?` is ordered, so the reversed spelling misses.
    try std.testing.expect(
        !implementation.isBanned("/* see internal the header for the documented contract. */"),
    );
}

test "invalid utf8 in a line answers quiet rather than trapping" {
    try std.testing.expect(!implementation.isBanned("/* see header \xff for the documented contract. */"));
}

test "fold maps ascii upper case and every folded code point" {
    try std.testing.expectEqual(@as(u21, 'a'), implementation.fold('A'));
    try std.testing.expectEqual(@as(u21, 'z'), implementation.fold('z'));
    try std.testing.expectEqual(@as(u21, 'i'), implementation.fold(0x0130));
    try std.testing.expectEqual(@as(u21, 'i'), implementation.fold(0x0131));
    try std.testing.expectEqual(@as(u21, 's'), implementation.fold(0x017F));
    try std.testing.expectEqual(@as(u21, 'k'), implementation.fold(0x212A));
}

test "a long s reads as s, as the case-insensitive rule always did" {
    try std.testing.expect(implementation.isBanned("/* \u{017F}ee header for the documented contract. */"));
}

test "a non-breaking space counts as detector whitespace" {
    try std.testing.expect(implementation.isRegexSpace(0x00A0));
    try std.testing.expect(implementation.isRegexSpace(0x3000));
    try std.testing.expect(implementation.isRegexSpace(' '));
    try std.testing.expect(!implementation.isRegexSpace('x'));
    try std.testing.expect(implementation.isBanned("\u{00A0}/* see header for the documented contract. */"));
}

test "line breaks follow the splitlines set" {
    try std.testing.expect(implementation.isLineBreak('\n'));
    try std.testing.expect(implementation.isLineBreak(0x0B));
    try std.testing.expect(implementation.isLineBreak(0x0C));
    try std.testing.expect(implementation.isLineBreak(0x1C));
    try std.testing.expect(implementation.isLineBreak(0x85));
    try std.testing.expect(implementation.isLineBreak(0x2028));
    try std.testing.expect(!implementation.isLineBreak('\t'));
}

test "the line iterator splits on lf and keeps no trailing empty line" {
    var lines = implementation.LineIterator.init("a\nb\n");
    try std.testing.expectEqualStrings("a", lines.next().?);
    try std.testing.expectEqualStrings("b", lines.next().?);
    try std.testing.expect(lines.next() == null);
}

test "the line iterator treats crlf as one break" {
    var lines = implementation.LineIterator.init("a\r\nb\rc");
    try std.testing.expectEqualStrings("a", lines.next().?);
    try std.testing.expectEqualStrings("b", lines.next().?);
    try std.testing.expectEqualStrings("c", lines.next().?);
    try std.testing.expect(lines.next() == null);
}

test "the line iterator breaks on a form feed" {
    var lines = implementation.LineIterator.init("a\x0Cb");
    try std.testing.expectEqualStrings("a", lines.next().?);
    try std.testing.expectEqualStrings("b", lines.next().?);
    try std.testing.expect(lines.next() == null);
}

test "scanText numbers findings from one" {
    const text =
        "void f(void) {\n" ++
        "/* see header for the documented contract. */\n" ++
        "}\n";
    const hits = try implementation.scanText(std.testing.allocator, text);
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{2}, hits);
}

test "scanText counts a form feed as a line break when numbering" {
    const text = "a\x0C/* see header for the documented contract. */\n";
    const hits = try implementation.scanText(std.testing.allocator, text);
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{2}, hits);
}

test "scanText reports every offending line in order" {
    const text =
        "/* see header for the documented contract. */\n" ++
        "int x;\n" ++
        "  /* See the internal header for the documented contract. */  \n";
    const hits = try implementation.scanText(std.testing.allocator, text);
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{ 1, 3 }, hits);
}

test "scanText answers empty on a clean source" {
    const hits = try implementation.scanText(std.testing.allocator, "int main(void) { return 0; }\n");
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqual(@as(usize, 0), hits.len);
}

test "renderFinding prints path and line" {
    const rendered = try implementation.renderFinding(std.testing.allocator, "apps/a/src/main.c", 12);
    defer std.testing.allocator.free(rendered);
    try std.testing.expectEqualStrings("apps/a/src/main.c:12", rendered);
}

test "pathSuffix follows pathlib semantics" {
    try std.testing.expectEqualStrings(".c", implementation.pathSuffix("apps/a/src/main.c"));
    try std.testing.expectEqualStrings(".hpp", implementation.pathSuffix("examples/b/inc/x.y.hpp"));
    try std.testing.expectEqualStrings("", implementation.pathSuffix("apps/a/.clang-format"));
    try std.testing.expectEqualStrings("", implementation.pathSuffix("apps/a/trailing."));
    try std.testing.expectEqualStrings("", implementation.pathSuffix("apps/a/Makefile"));
}

test "source suffixes are matched case folded" {
    try std.testing.expect(implementation.hasSourceSuffix("apps/a/src/main.C"));
    try std.testing.expect(implementation.hasSourceSuffix("apps/a/inc/api.H"));
    try std.testing.expect(implementation.hasSourceSuffix("examples/b/src/objc.mm"));
    try std.testing.expect(!implementation.hasSourceSuffix("apps/a/src/build.py"));
    try std.testing.expect(!implementation.hasSourceSuffix("apps/a/src/notes.md"));
}

test "scope covers apps and examples only" {
    try std.testing.expect(implementation.isScopedPrefix("apps/a/src/main.c"));
    try std.testing.expect(implementation.isScopedPrefix("examples/b/src/main.c"));
    try std.testing.expect(!implementation.isScopedPrefix("libs/ra8_ui/src/ui.c"));
    try std.testing.expect(!implementation.isScopedPrefix("tools/x/src/main.c"));
}

test "selectScoped filters prefix, suffix and existence, then sorts" {
    const census = [_][]const u8{
        "examples/b/src/main.c",
        "apps/a/src/main.c",
        "apps/a/README.md",
        "libs/c/src/lib.c",
        "apps/a/src/gone.c",
    };
    const present = [_][]const u8{
        "examples/b/src/main.c",
        "apps/a/src/main.c",
        "apps/a/README.md",
        "libs/c/src/lib.c",
    };
    var oracle = ListResolver{ .present = &present };
    const scoped = try implementation.selectScoped(std.testing.allocator, &census, oracle.resolver());
    defer std.testing.allocator.free(scoped);
    try std.testing.expectEqual(@as(usize, 2), scoped.len);
    try std.testing.expectEqualStrings("apps/a/src/main.c", scoped[0]);
    try std.testing.expectEqualStrings("examples/b/src/main.c", scoped[1]);
}

test "selectScoped de-duplicates a repeated census path" {
    const census = [_][]const u8{ "apps/a/src/main.c", "apps/a/src/main.c" };
    const present = [_][]const u8{"apps/a/src/main.c"};
    var oracle = ListResolver{ .present = &present };
    const scoped = try implementation.selectScoped(std.testing.allocator, &census, oracle.resolver());
    defer std.testing.allocator.free(scoped);
    try std.testing.expectEqual(@as(usize, 1), scoped.len);
}

test "selectScoped keeps a dot-prefixed directory in scope" {
    // `pathlib.Path.glob` never hid dot-prefixed names and neither did the
    // census, so a hidden directory is scanned, not silently dropped.
    const census = [_][]const u8{"apps/.hidden/src/main.c"};
    const present = [_][]const u8{"apps/.hidden/src/main.c"};
    var oracle = ListResolver{ .present = &present };
    const scoped = try implementation.selectScoped(std.testing.allocator, &census, oracle.resolver());
    defer std.testing.allocator.free(scoped);
    try std.testing.expectEqual(@as(usize, 1), scoped.len);
}

test "selectScoped drops the empty census trailer" {
    const census = [_][]const u8{ "apps/a/src/main.c", "" };
    const present = [_][]const u8{"apps/a/src/main.c"};
    var oracle = ListResolver{ .present = &present };
    const scoped = try implementation.selectScoped(std.testing.allocator, &census, oracle.resolver());
    defer std.testing.allocator.free(scoped);
    try std.testing.expectEqual(@as(usize, 1), scoped.len);
}

test "the scope floor is an error below the documented minimum" {
    try std.testing.expect(implementation.scopeCollapsed(849, implementation.min_scoped_files));
    try std.testing.expect(!implementation.scopeCollapsed(850, implementation.min_scoped_files));
    try std.testing.expectEqual(@as(usize, 850), implementation.min_scoped_files);
}

test "the detector selftest passes in both directions" {
    const failures = try implementation.selftestFailures(std.testing.allocator);
    defer std.testing.allocator.free(failures);
    try std.testing.expectEqual(@as(usize, 0), failures.len);
    try std.testing.expectEqual(@as(usize, 5), implementation.selftest_cases.len);
}

test "a dotless i in the internal word still fires, as re.IGNORECASE did" {
    // Measured against the interpreter: re.IGNORECASE matches U+0131 against
    // `i`, because both share LATIN CAPITAL LETTER I. Folding it away would
    // let the banned sentence through the gate one substitution at a time.
    try std.testing.expect(
        implementation.isBanned("/* see \u{0131}nternal header for the documented contract. */"),
    );
}

test "a dotted capital I in the internal word still fires" {
    try std.testing.expect(
        implementation.isBanned("/* see \u{0130}nternal header for the documented contract. */"),
    );
    // The fold is not a licence to match any letter with a mark: a small i
    // with an acute accent is a different letter and stays quiet.
    try std.testing.expect(
        !implementation.isBanned("/* see \u{00ED}nternal header for the documented contract. */"),
    );
}
