//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the TrustZone boot-boundary discard rules
//! (#1250). Every case here was probed against the predecessor
//! `scripts/checks/check_tz_boundary_discard.py` under CPython 3.11 before it
//! was written down, so a divergence is a real behaviour change and not a
//! guess about what the Python "meant".

const std = @import("std");
const testing = std.testing;
const impl = @import("implementation");

fn scan(text: []const u8, is_c: bool) ![]impl.Finding {
    return impl.scanText(testing.allocator, text, is_c);
}

fn rules(text: []const u8, is_c: bool) ![]impl.Rule {
    const findings = try scan(text, is_c);
    defer testing.allocator.free(findings);
    const out = try testing.allocator.alloc(impl.Rule, findings.len);
    for (findings, 0..) |finding, index| out[index] = finding.rule;
    return out;
}

fn expectRules(text: []const u8, is_c: bool, expected: []const impl.Rule) !void {
    const found = try rules(text, is_c);
    defer testing.allocator.free(found);
    try testing.expectEqualSlices(impl.Rule, expected, found);
}

// --- constants the registries used to name -------------------------------

test "the file floor stays where it was measured" {
    try testing.expectEqual(@as(usize, 1700), impl.file_floor);
}

test "the scanned roots are the six the predecessor walked" {
    try testing.expectEqual(@as(usize, 6), impl.roots.len);
    try testing.expectEqualStrings("libs", impl.roots[0]);
    try testing.expectEqualStrings("apps", impl.roots[5]);
}

test "the scanned suffixes are C and C++ sources and headers" {
    try testing.expectEqual(@as(usize, 4), impl.exts.len);
    try testing.expect(impl.hasScannedExt("libs/a.c"));
    try testing.expect(impl.hasScannedExt("libs/a.h"));
    try testing.expect(impl.hasScannedExt("libs/a.cpp"));
    try testing.expect(impl.hasScannedExt("libs/a.hpp"));
}

test "other suffixes are out of scope" {
    try testing.expect(!impl.hasScannedExt("libs/a.cc"));
    try testing.expect(!impl.hasScannedExt("libs/a.zig"));
    try testing.expect(!impl.hasScannedExt("libs/a.py"));
}

test "a bare suffix name still counts, as the glob did" {
    try testing.expect(impl.hasScannedExt("libs/.c"));
}

test "vendored SOUP and generated font data are exempt" {
    try testing.expect(impl.isExempt("libs/third_party/lvgl/lv.c"));
    try testing.expect(impl.isExempt("libs/ra8_fonts/gen.c"));
    try testing.expect(!impl.isExempt("libs/ra8_tz/tz.c"));
}

test "the exempt fragments are slash-wrapped, so a prefix match is not enough" {
    try testing.expect(!impl.isExempt("libs/third_party_shim/a.c"));
    try testing.expect(!impl.isExempt("third_party/a.c"));
}

test "only a .c file can be a boot translation unit" {
    try testing.expect(impl.isCFile("libs/boot.c"));
    try testing.expect(!impl.isCFile("libs/boot.h"));
    try testing.expect(!impl.isCFile("libs/boot.cpp"));
}

// --- rule A: the world-switch family, anywhere ---------------------------

test "a world-switch discard fires in an ordinary file" {
    try expectRules("void f(void) { (void)ra8_tz_secure_boot_verify(); }\n", false, &.{.a});
}

test "a world-switch discard fires in a header too" {
    try expectRules("static inline void f(void) { (void)ra8_tz_secure_boot_arm(); }\n", false, &.{.a});
}

test "a handled world-switch call is quiet" {
    try expectRules("if (ra8_tz_secure_boot_verify() != k_ra8_ok) { halt(); }\n", false, &.{});
}

test "spaces inside the cast do not hide the discard" {
    try expectRules("( void )ra8_tz_secure_boot_verify();\n", false, &.{.a});
}

test "whitespace between the cast and the call does not hide it" {
    try expectRules("(void)   ra8_tz_secure_boot_verify();\n", false, &.{.a});
}

test "a tab between the cast and the call does not hide it" {
    try expectRules("(void)\tra8_tz_secure_boot_verify();\n", false, &.{.a});
}

test "whitespace before the call parenthesis does not hide it" {
    try expectRules("(void)ra8_tz_secure_boot_verify ();\n", false, &.{.a});
}

test "a non-breaking space counts as whitespace, as re did" {
    try expectRules("(void)\u{00a0}ra8_tz_secure_boot_verify();\n", false, &.{.a});
}

test "the family needs at least one character after its prefix" {
    try expectRules("(void)ra8_tz_secure_boot_();\n", false, &.{});
}

test "a function-pointer discard with no call parentheses is not a finding" {
    try expectRules("(void)ra8_tz_secure_boot_verify;\n", false, &.{});
}

test "an uppercase family name is not the family" {
    try expectRules("(void)ra8_tz_secure_boot_VERIFY();\n", false, &.{});
}

test "two world-switch discards on one line are two findings" {
    try expectRules("(void)ra8_tz_secure_boot_a(); (void)ra8_tz_secure_boot_b();\n", false, &.{ .a, .a });
}

test "a discarded non-family ra8 call is quiet outside a boot TU" {
    try expectRules("(void)ra8_cgc_init();\n", false, &.{});
}

test "a discarded non-ra8 call is never a finding" {
    try expectRules("(void)memset(p, 0, n);\n", true, &.{});
}

// --- rule B: any ra8_ call inside a boot translation unit -----------------

test "a boot TU defined by SystemInit reports any ra8 discard" {
    try expectRules("void SystemInit(void) { (void)ra8_cgc_init(); }\n", true, &.{.b});
}

test "a boot TU defined by ra8_trustzone_init reports any ra8 discard" {
    try expectRules("void ra8_trustzone_init(void) { (void)ra8_sau_apply(); }\n", true, &.{.b});
}

test "the same text in a header is not a boot TU" {
    try expectRules("void SystemInit(void) { (void)ra8_cgc_init(); }\n", false, &.{});
}

test "an indented boot entry point still marks the TU" {
    // Two findings, not one: the entry-point line itself ENDS in `(void)`, so
    // the predecessor's dangling-cast join glued the body onto it and then
    // scanned the body line again on its own. Probed against CPython.
    try expectRules("    void SystemInit(void)\n{ (void)ra8_cgc_init(); }\n", true, &.{ .b, .b });
}

test "a boot entry point on a later line still marks the TU" {
    try expectRules("#include <ra8.h>\nvoid SystemInit(void) {}\nvoid g(void) { (void)ra8_mpu_apply(); }\n", true, &.{.b});
}

test "a boot entry point taking arguments does not mark the TU" {
    try expectRules("void SystemInit(int mode) { (void)ra8_cgc_init(); }\n", true, &.{});
}

test "a longer identifier starting with the entry-point name does not mark the TU" {
    try expectRules("void SystemInitEarly(void) { (void)ra8_cgc_init(); }\n", true, &.{});
}

test "a boot entry point must be preceded only by whitespace on its line" {
    try expectRules("static void SystemInit(void) { (void)ra8_cgc_init(); }\n", true, &.{});
}

test "avoid is not void at a line start" {
    try testing.expect(!impl.matchBootEntryAt("  avoid SystemInit(void)", 0));
}

test "the boot marker needs whitespace after void" {
    try testing.expect(!impl.matchBootEntryAt("voidSystemInit(void)", 0));
}

test "the boot marker accepts whitespace around its parameter list" {
    try testing.expect(impl.matchBootEntryAt("void  SystemInit ( void )", 0));
}

test "the multiline anchor follows a newline only" {
    try testing.expect(impl.bootTuMatch("x\nvoid SystemInit(void)"));
    try testing.expect(!impl.bootTuMatch("x\rvoid SystemInit(void)"));
}

test "a world-switch discard inside a boot TU is reported once, under rule A" {
    try expectRules("void SystemInit(void) { (void)ra8_tz_secure_boot_verify(); }\n", true, &.{.a});
}

test "rule A findings precede rule B findings on the same line" {
    try expectRules(
        "void SystemInit(void) { (void)ra8_cgc_init(); (void)ra8_tz_secure_boot_verify(); }\n",
        true,
        &.{ .a, .b },
    );
}

// --- waivers --------------------------------------------------------------

test "a reasoned waiver silences the line" {
    try expectRules("(void)ra8_tz_secure_boot_verify(); /* TZ-DISCARD-OK: documented */\n", false, &.{});
}

test "a waiver with no reason text does not silence the line" {
    try expectRules("(void)ra8_tz_secure_boot_verify(); /* TZ-DISCARD-OK:", false, &.{.a});
}

test "a waiver marker with trailing whitespace only does not silence the line" {
    try expectRules("(void)ra8_tz_secure_boot_verify(); // TZ-DISCARD-OK:   ", false, &.{.a});
}

test "the waiver is recognised anywhere on the line" {
    try expectRules("/* TZ-DISCARD-OK: leading */ (void)ra8_tz_secure_boot_verify();\n", false, &.{});
}

test "a later reasoned waiver satisfies the search after an empty one" {
    try testing.expect(impl.hasWaiver("TZ-DISCARD-OK: TZ-DISCARD-OK: why"));
}

test "a lowercase marker is not the waiver" {
    try testing.expect(!impl.hasWaiver("tz-discard-ok: why"));
}

test "the waiver reason may be separated by a tab" {
    try testing.expect(impl.hasWaiver("TZ-DISCARD-OK:\twhy"));
}

test "the waiver applies to the joined line, not just the raw one" {
    try expectRules("(void)\nra8_tz_secure_boot_verify(); /* TZ-DISCARD-OK: joined */\n", false, &.{});
}

// --- comments -------------------------------------------------------------

test "a line comment before the cast exempts it" {
    try expectRules("// (void)ra8_tz_secure_boot_verify();\n", false, &.{});
}

test "a block comment opener before the cast exempts it" {
    try expectRules("/* (void)ra8_tz_secure_boot_verify(); */\n", false, &.{});
}

test "a continuation asterisk line exempts the cast" {
    try expectRules(" * (void)ra8_tz_secure_boot_verify();\n", false, &.{});
}

test "a trailing comment does not exempt code before it" {
    try expectRules("(void)ra8_tz_secure_boot_verify(); // why\n", false, &.{.a});
}

test "a line that opens with a block comment exempts the whole line" {
    // The predecessor tested `line.lstrip().startswith(("*", "//", "/*"))`, so
    // a closed `/* note */` at the START of the line exempts code after it.
    try expectRules("/* note */ (void)ra8_tz_secure_boot_verify();\n", false, &.{});
}

test "a closed block comment mid-line does not exempt the cast after it" {
    try expectRules("x /* note */ (void)ra8_tz_secure_boot_verify();\n", false, &.{.a});
}

test "the comment test reads the whole prefix, crude as it is" {
    try testing.expect(impl.isCommentPos("x // y (void)", 7));
    try testing.expect(!impl.isCommentPos("x (void)", 2));
    try testing.expect(impl.isCommentPos("x /* y (void)", 7));
    try testing.expect(!impl.isCommentPos("x /* y */ (void)", 10));
}

// --- the split-cast join --------------------------------------------------

test "a cast left dangling at end of line joins the next line" {
    try expectRules("(void)\nra8_tz_secure_boot_verify();\n", false, &.{.a});
}

test "the join reports the line the cast sits on" {
    const findings = try scan("x;\n(void)\nra8_tz_secure_boot_verify();\n", false);
    defer testing.allocator.free(findings);
    try testing.expectEqual(@as(usize, 1), findings.len);
    try testing.expectEqual(@as(usize, 2), findings[0].line);
}

test "a dangling cast on the last line has nothing to join" {
    try expectRules("ra8_tz_secure_boot_verify();\n(void)", false, &.{});
}

test "the join needs the cast at end of line" {
    try testing.expect(impl.endsWithVoidCast("  (void)  "));
    try testing.expect(impl.endsWithVoidCast("x ( void )"));
    try testing.expect(!impl.endsWithVoidCast("(void)x"));
    try testing.expect(!impl.endsWithVoidCast("void)"));
}

test "the joined line carries no separator, as the predecessor concatenated it" {
    try expectRules("(void)\n  ra8_tz_secure_boot_verify();\n", false, &.{.a});
}

// --- the fast path and decoding ------------------------------------------

test "a file with no cast at all is skipped by the fast path" {
    try testing.expect(!impl.hasVoidIgnoringSpaces("ra8_tz_secure_boot_verify();\n"));
}

test "the fast path ignores spaces only, not tabs" {
    try testing.expect(impl.hasVoidIgnoringSpaces("( v o i d )"));
    try testing.expect(!impl.hasVoidIgnoringSpaces("(\tvoid)"));
}

test "the fast path restarts on a repeated opening parenthesis" {
    try testing.expect(impl.hasVoidIgnoringSpaces("((void)"));
}

test "a tab-separated cast still reaches the scan through the fast path" {
    try expectRules("(void)\tra8_tz_secure_boot_verify();\n", false, &.{.a});
}

// --- line splitting -------------------------------------------------------

test "splitlines honours every boundary CPython honours" {
    var iterator = impl.LineIterator{ .text = "a\nb\rc\r\nd\x0be\x0cf\x1cg\x1dh\x1ei\u{0085}j\u{2028}k" };
    var count: usize = 0;
    while (iterator.next()) |_| count += 1;
    try testing.expectEqual(@as(usize, 11), count);
}

test "a trailing newline adds no empty line" {
    var iterator = impl.LineIterator{ .text = "a\nb\n" };
    try testing.expectEqualStrings("a", iterator.next().?);
    try testing.expectEqualStrings("b", iterator.next().?);
    try testing.expect(iterator.next() == null);
}

test "a carriage-return newline pair is one boundary" {
    try testing.expectEqual(@as(usize, 2), impl.lineBreakLen("a\r\nb", 1));
    try testing.expectEqual(@as(usize, 1), impl.lineBreakLen("a\rb", 1));
    try testing.expectEqual(@as(usize, 0), impl.lineBreakLen("ab", 1));
}

test "a finding on a later line reports its own number" {
    const findings = try scan("a;\nb;\n(void)ra8_tz_secure_boot_verify();\n", false);
    defer testing.allocator.free(findings);
    try testing.expectEqual(@as(usize, 3), findings[0].line);
}

// --- snippets -------------------------------------------------------------

test "the snippet is the stripped line" {
    const findings = try scan("   (void)ra8_tz_secure_boot_verify();   \n", false);
    defer testing.allocator.free(findings);
    try testing.expectEqualStrings("(void)ra8_tz_secure_boot_verify();", findings[0].snippet);
}

test "the snippet truncates at a hundred code points, not bytes" {
    const padding = "\u{00e9}" ** 200;
    const findings = try scan("(void)ra8_tz_secure_boot_verify(); // " ++ padding ++ "\n", false);
    defer testing.allocator.free(findings);
    try testing.expectEqual(@as(usize, 100), try std.unicode.utf8CountCodepoints(findings[0].snippet));
}

test "the snippet of a joined line is the raw line only" {
    const findings = try scan("(void)\nra8_tz_secure_boot_verify();\n", false);
    defer testing.allocator.free(findings);
    try testing.expectEqualStrings("(void)", findings[0].snippet);
}

test "strip and lstrip use CPython's whitespace table" {
    try testing.expectEqualStrings("x", impl.strip("\u{00a0}\t x \n"));
    try testing.expectEqualStrings("x ", impl.lstrip("\u{3000}x "));
}

// --- build output and path normalisation ---------------------------------

test "a build directory component is build output" {
    try testing.expect(impl.isBuildOutput("tests/build/a.c"));
    try testing.expect(impl.isBuildOutput("examples/blink/build-cov/a.c"));
    try testing.expect(impl.isBuildOutput("tools/vela/cmake-build-debug/a.c"));
}

test "builders is not a build directory" {
    try testing.expect(!impl.isBuildOutput("scripts/builders/a.c"));
    try testing.expect(!impl.isBuildDirName("builders"));
}

test "a build directory under an unlisted root only counts at depth zero" {
    try testing.expect(impl.isBuildOutput("build/a.c"));
    try testing.expect(!impl.isBuildOutput("libs/ra8_ui/build/a.c"));
}

test "a tool output directory counts at any depth" {
    try testing.expect(impl.isBuildOutput("libs/ra8_ui/__pycache__/a.c"));
    try testing.expect(impl.isBuildOutput("libs/x/.zig-cache/a.c"));
    try testing.expect(impl.isBuildOutput("libs/x/CMakeFiles/a.c"));
}

test "a file named build is not a build tree" {
    try testing.expect(!impl.isBuildOutput("libs/build"));
}

test "an absolute path under the repo root normalises before the test" {
    try testing.expect(impl.isBuildOutputPath("/repo/tests/build/a.c", "/repo"));
    try testing.expect(!impl.isBuildOutputPath("/repo/libs/a.c", "/repo"));
}

test "a dot-slash prefix normalises too" {
    try testing.expect(impl.isBuildOutputPath("./tests/build/a.c", "/repo"));
}

test "slash wrapping is stripped from both ends" {
    try testing.expectEqualStrings("a/b", impl.stripSlashes("//a/b//"));
}

test "sorting is code-point order" {
    try testing.expect(impl.pythonLessThan({}, "libs/a.c", "libs/b.c"));
    try testing.expect(impl.pythonLessThan({}, "libs/Z.c", "libs/a.c"));
}

// --- rendering ------------------------------------------------------------

fn render(comptime what: enum { finding, summary, clean, floor, usage }, value: usize) ![]u8 {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    errdefer buffer.deinit();
    switch (what) {
        .finding => try impl.renderFinding(buffer.writer(), "libs/x.c", .{
            .line = value,
            .rule = .a,
            .snippet = "(void)ra8_tz_secure_boot_verify();",
        }),
        .summary => try impl.renderSummary(buffer.writer(), value),
        .clean => try impl.renderClean(buffer.writer()),
        .floor => try impl.renderFloor(buffer.writer(), value),
        .usage => try impl.renderUsage(buffer.writer()),
    }
    return buffer.toOwnedSlice();
}

test "a finding line names the path, line, rule and guidance" {
    const text = try render(.finding, 12);
    defer testing.allocator.free(text);
    try testing.expectEqualStrings(
        "libs/x.c:12: [rule A] world-switch result discarded -- handle the ra8_err_t " ++
            "(halt or a documented fallback; RA8_ERROR_CHECK[_NO_ABORT]); " ++
            "never (void)-cast it at a TrustZone boot boundary; (void)ra8_tz_secure_boot_verify();\n",
        text,
    );
}

test "the rule B clause names the boot translation unit" {
    try testing.expectEqualStrings("boot-TU ra8_* result discarded", impl.Rule.b.what());
    try testing.expectEqualStrings("B", impl.Rule.b.label());
}

test "the summary opens with a blank line and counts violations" {
    const text = try render(.summary, 3);
    defer testing.allocator.free(text);
    try testing.expect(std.mem.startsWith(u8, text, "\ncheck_tz_boundary_discard: 3 violation(s)."));
    try testing.expect(std.mem.indexOf(u8, text, "`TZ-DISCARD-OK: <reason>`") != null);
}

test "the clean line names the gate" {
    const text = try render(.clean, 0);
    defer testing.allocator.free(text);
    try testing.expectEqualStrings(
        "check_tz_boundary_discard: clean -- no silent ra8_err_t discards at TrustZone boot boundaries.\n",
        text,
    );
}

test "the floor line explains why a collapsed sweep is fatal" {
    const text = try render(.floor, 12);
    defer testing.allocator.free(text);
    try testing.expectEqualStrings(
        "check_tz_boundary_discard: FATAL -- only 12 first-party source file(s) in scope, " ++
            "floor is 1700. A collapsed sweep reports a clean tree because it scanned nothing.\n",
        text,
    );
}

test "the usage line lists the only accepted flag" {
    const text = try render(.usage, 0);
    defer testing.allocator.free(text);
    try testing.expectEqualStrings("usage: check_tz_boundary_discard [--selftest] [file ...]\n", text);
}

// --- selftest -------------------------------------------------------------

test "the selftest holds in both directions" {
    const outcome = try impl.runSelftest(testing.allocator);
    for (outcome.passed) |passed| try testing.expect(passed);
}

test "the selftest fixtures are the predecessor's" {
    try expectRules(impl.family_fixture, true, &.{.a});
    try expectRules(impl.boot_fixture, true, &.{.b});
    try expectRules(impl.good_fixture, true, &.{});
}

test "a passing selftest prints both cases and exits zero" {
    var out = std.ArrayList(u8).init(testing.allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(testing.allocator);
    defer err.deinit();
    const outcome = impl.SelftestOutcome{ .passed = .{ true, true } };
    try testing.expectEqual(@as(u8, 0), try impl.renderSelftest(out.writer(), err.writer(), outcome));
    try testing.expect(std.mem.indexOf(u8, out.items, "  [ok] world-switch") != null);
    try testing.expect(std.mem.endsWith(u8, out.items, "all cases pass (both directions).\n"));
    try testing.expectEqualStrings("", err.items);
}

test "a failing selftest names the count on stderr and exits one" {
    var out = std.ArrayList(u8).init(testing.allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(testing.allocator);
    defer err.deinit();
    const outcome = impl.SelftestOutcome{ .passed = .{ false, true } };
    try testing.expectEqual(@as(u8, 1), try impl.renderSelftest(out.writer(), err.writer(), outcome));
    try testing.expect(std.mem.indexOf(u8, out.items, "  [FAIL] world-switch") != null);
    try testing.expectEqualStrings("check_tz_boundary_discard --selftest: 1 failure(s)\n", err.items);
}

// --- whitespace tables ----------------------------------------------------

test "the re and str whitespace tables agree on this interpreter" {
    try testing.expectEqual(impl.char_classes.re_space_intervals.len, impl.char_classes.str_space_intervals.len);
    var code_point: u21 = 0;
    while (code_point < 0x3100) : (code_point += 1) {
        try testing.expectEqual(impl.isReSpace(code_point), impl.isStrSpace(code_point));
    }
}

test "the information separators are whitespace, as CPython has them" {
    try testing.expect(impl.isReSpace(0x1C));
    try testing.expect(impl.isReSpace(0x1F));
    try testing.expect(!impl.isReSpace(0x1B));
}

test "an ideographic space is whitespace and a zero-width space is not" {
    try testing.expect(impl.isReSpace(0x3000));
    try testing.expect(!impl.isReSpace(0x200B));
}

test "decoding walks forwards and backwards over the same code point" {
    const text = "a\u{2028}b";
    try testing.expectEqual(@as(u21, 0x2028), impl.decodeAt(text, 1).code_point);
    try testing.expectEqual(@as(usize, 3), impl.decodeAt(text, 1).len);
    try testing.expectEqual(@as(u21, 0x2028), impl.decodeBefore(text, 4).code_point);
}

test "a space run skips multi-byte whitespace in both directions" {
    const text = "(\u{00a0}\u{3000})";
    try testing.expectEqual(@as(usize, 6), impl.spaceRun(text, 1));
    try testing.expectEqual(@as(usize, 1), impl.spaceRunBackward(text, 6));
}
