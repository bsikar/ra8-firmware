//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Declaration and definition algebra of the NSC veneer gate (#858). Every
//! case here is pure text, so the two patterns the Python gate carried as
//! regular expressions are pinned with no repository on disk.

const std = @import("std");
const implementation = @import("implementation");

/// Collect the declared names of one header text.
fn declared(text: []const u8) ![]const []const u8 {
    return implementation.declaredVeneers(std.testing.allocator, text);
}

/// Free a collected name list.
fn release(names: []const []const u8) void {
    std.testing.allocator.free(names);
}

test "a declaration with a return type is captured" {
    const names = try declared("[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_xspi_read(uint32_t off);\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 1), names.len);
    try std.testing.expectEqualStrings("ra8_nsc_xspi_read", names[0]);
}

test "a void veneer is captured like any other" {
    const names = try declared("RA8_NSC_VENEER void ra8_nsc_wdt_refresh(void);\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 1), names.len);
    try std.testing.expectEqualStrings("ra8_nsc_wdt_refresh", names[0]);
}

test "a pointer return type is captured, the star is in the class" {
    const names = try declared("RA8_NSC_VENEER ra8_err_t* ra8_nsc_ptr(void);\n");
    defer release(names);
    try std.testing.expectEqualStrings("ra8_nsc_ptr", names[0]);
}

test "whitespace between the name and the parenthesis is allowed" {
    const names = try declared("RA8_NSC_VENEER ra8_err_t ra8_nsc_spaced \t (void);\n");
    defer release(names);
    try std.testing.expectEqualStrings("ra8_nsc_spaced", names[0]);
}

test "a declaration split across lines is captured" {
    const names = try declared("RA8_NSC_VENEER\nra8_err_t\nra8_nsc_wrapped(void);\n");
    defer release(names);
    try std.testing.expectEqualStrings("ra8_nsc_wrapped", names[0]);
}

test "a declaration with no return type is invisible, as it always was" {
    // Inherited behaviour, not a choice: the pattern needs a mandatory word
    // character after the macro, and the only candidate is the name itself.
    const names = try declared("RA8_NSC_VENEER ra8_nsc_bare(void);\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 0), names.len);
}

test "the macro must be followed by whitespace" {
    const names = try declared("RA8_NSC_VENEERra8_err_t ra8_nsc_glued(void);\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 0), names.len);
}

test "the class cannot cross a semicolon into a later call" {
    const names = try declared("RA8_NSC_VENEER int x; ra8_nsc_call(0);\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 0), names.len);
}

test "the class cannot cross a comma" {
    const names = try declared("RA8_NSC_VENEER ra8_err_t a, ra8_nsc_after(void);\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 0), names.len);
}

test "a bare call with no macro is not a declaration" {
    const names = try declared("void caller(void) { ra8_nsc_phantom(); }\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 0), names.len);
}

test "two declarations are captured in order" {
    const names = try declared(
        "RA8_NSC_VENEER ra8_err_t ra8_nsc_alpha(void);\nRA8_NSC_VENEER void ra8_nsc_beta(void);\n",
    );
    defer release(names);
    try std.testing.expectEqual(@as(usize, 2), names.len);
    try std.testing.expectEqualStrings("ra8_nsc_alpha", names[0]);
    try std.testing.expectEqualStrings("ra8_nsc_beta", names[1]);
}

test "a repeated declaration is reported once, in first-seen order" {
    const names = try declared(
        "RA8_NSC_VENEER ra8_err_t ra8_nsc_dup(void);\n" ++
            "RA8_NSC_VENEER ra8_err_t ra8_nsc_other(void);\n" ++
            "RA8_NSC_VENEER ra8_err_t ra8_nsc_dup(uint8_t a);\n",
    );
    defer release(names);
    try std.testing.expectEqual(@as(usize, 2), names.len);
    try std.testing.expectEqualStrings("ra8_nsc_dup", names[0]);
    try std.testing.expectEqualStrings("ra8_nsc_other", names[1]);
}

test "the name needs at least one character after the prefix" {
    const names = try declared("RA8_NSC_VENEER ra8_err_t ra8_nsc_(void);\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 0), names.len);
}

test "a name embedded in a longer word does not start at a boundary" {
    const names = try declared("RA8_NSC_VENEER ra8_err_t xra8_nsc_inner(void);\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 0), names.len);
}

test "the documentation prose that names the macro is not a declaration" {
    const names = try declared(" * ``RA8_NSC_VENEER`` (see ``ra8_nsc_veneer.h``) expands to the real\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 0), names.len);
}

test "a match ends past the parenthesis so scanning does not repeat it" {
    const match = implementation.nextMatch(
        "RA8_NSC_VENEER ra8_err_t ra8_nsc_one(void);",
        0,
        .any,
    ).?;
    try std.testing.expectEqual(@as(usize, 0), match.start);
    try std.testing.expectEqual(@as(usize, 37), match.end);
    try std.testing.expectEqualStrings("ra8_nsc_one", match.name);
}

test "the real header shape from this tree is captured" {
    const names = try declared(
        "[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_key_vault_challenge(uint16_t       slot,\n" ++
            "                                                              uint8_t* out_tag);\n",
    );
    defer release(names);
    try std.testing.expectEqualStrings("ra8_nsc_key_vault_challenge", names[0]);
}

test "a definition with a body is found" {
    try std.testing.expect(implementation.definesVeneer(
        "RA8_NSC_VENEER ra8_err_t ra8_nsc_defined(void) { return 0; }\n",
        "ra8_nsc_defined",
    ));
}

test "a call site with no macro is not a definition" {
    try std.testing.expect(!implementation.definesVeneer(
        "void caller(void) { ra8_nsc_phantom(); }\n",
        "ra8_nsc_phantom",
    ));
}

test "a longer name is not a definition of the shorter one" {
    // The Python pattern had no trailing boundary, so this is pinned by the
    // `\\s*\\(` tail alone: `2` is neither whitespace nor the parenthesis.
    try std.testing.expect(!implementation.definesVeneer(
        "RA8_NSC_VENEER ra8_err_t ra8_nsc_target2(void) { return 0; }\n",
        "ra8_nsc_target",
    ));
}

test "a definition head may reach across an earlier veneer name" {
    // The class crosses word characters, so the definition search is not
    // stopped by another name the way the capturing scan is.
    try std.testing.expect(implementation.definesVeneer(
        "RA8_NSC_VENEER ra8_err_t ra8_nsc_other ra8_nsc_target(void) { return 0; }\n",
        "ra8_nsc_target",
    ));
}

test "a definition search cannot cross a statement end" {
    try std.testing.expect(!implementation.definesVeneer(
        "RA8_NSC_VENEER ra8_err_t x; ra8_nsc_target(0);\n",
        "ra8_nsc_target",
    ));
}

test "a definition split across lines is found" {
    try std.testing.expect(implementation.definesVeneer(
        "RA8_NSC_VENEER\nvoid\nra8_nsc_wrapped(void)\n{\n}\n",
        "ra8_nsc_wrapped",
    ));
}

test "one source defining a different veneer does not define this one" {
    try std.testing.expect(!implementation.definesVeneer(
        "RA8_NSC_VENEER ra8_err_t ra8_nsc_eth_send(const uint8_t* f) { return 0; }\n",
        "ra8_nsc_eth_recv",
    ));
}

test "word bytes are the ASCII word class" {
    try std.testing.expect(implementation.isWordByte('a'));
    try std.testing.expect(implementation.isWordByte('Z'));
    try std.testing.expect(implementation.isWordByte('7'));
    try std.testing.expect(implementation.isWordByte('_'));
    try std.testing.expect(!implementation.isWordByte('*'));
    try std.testing.expect(!implementation.isWordByte(' '));
    try std.testing.expect(!implementation.isWordByte(0xC3));
}

test "the space class is Python's, control separators included" {
    try std.testing.expect(implementation.isPythonSpace(' '));
    try std.testing.expect(implementation.isPythonSpace('\t'));
    try std.testing.expect(implementation.isPythonSpace('\n'));
    try std.testing.expect(implementation.isPythonSpace(0x0b));
    try std.testing.expect(implementation.isPythonSpace(0x0c));
    try std.testing.expect(implementation.isPythonSpace(0x1c));
    try std.testing.expect(implementation.isPythonSpace(0x1f));
    try std.testing.expect(implementation.isPythonSpace(0x85));
    try std.testing.expect(implementation.isPythonSpace(0xa0));
    try std.testing.expect(implementation.isPythonSpace(0x3000));
    try std.testing.expect(!implementation.isPythonSpace('x'));
    try std.testing.expect(!implementation.isPythonSpace(0x2014));
}

test "a vertical tab separates the macro from the return type" {
    const names = try declared("RA8_NSC_VENEER\x0bra8_err_t ra8_nsc_vt(void);\n");
    defer release(names);
    try std.testing.expectEqualStrings("ra8_nsc_vt", names[0]);
}

test "a no-break space separates the macro like any other space" {
    const names = try declared("RA8_NSC_VENEER\u{00a0}ra8_err_t ra8_nsc_nbsp(void);\n");
    defer release(names);
    try std.testing.expectEqualStrings("ra8_nsc_nbsp", names[0]);
}

test "the class ends at a parenthesis" {
    try std.testing.expectEqual(@as(?usize, null), implementation.classLen("(", 0));
    try std.testing.expectEqual(@as(?usize, null), implementation.classLen(";", 0));
    try std.testing.expectEqual(@as(?usize, 1), implementation.classLen("*", 0));
    try std.testing.expectEqual(@as(?usize, 1), implementation.classLen("q", 0));
    try std.testing.expectEqual(@as(?usize, null), implementation.classLen("", 0));
}

test "the mandatory word position rejects whitespace" {
    try std.testing.expectEqual(@as(?usize, 1), implementation.wordStartLen("r", 0));
    try std.testing.expectEqual(@as(?usize, null), implementation.wordStartLen(" ", 0));
    try std.testing.expectEqual(@as(?usize, null), implementation.wordStartLen("\u{00a0}", 0));
}

test "a boundary holds at the start of the text and after punctuation" {
    try std.testing.expect(implementation.boundaryBefore("ra8_nsc_x", 0));
    try std.testing.expect(implementation.boundaryBefore(" ra8", 1));
    try std.testing.expect(!implementation.boundaryBefore("xra8", 1));
}

test "a code point decodes with its byte length" {
    const ascii = implementation.charAt("a", 0).?;
    try std.testing.expectEqual(@as(u21, 'a'), ascii.code_point);
    try std.testing.expectEqual(@as(usize, 1), ascii.len);
    const wide = implementation.charAt("\u{3000}", 0).?;
    try std.testing.expectEqual(@as(u21, 0x3000), wide.code_point);
    try std.testing.expectEqual(@as(usize, 3), wide.len);
    try std.testing.expectEqual(@as(?implementation.Char, null), implementation.charAt("", 0));
}

test "a missing veneer renders with both paths" {
    const line = try implementation.renderMissing(
        std.testing.allocator,
        "ra8_nsc_phantom",
        "libs/ra8_nsc/inc/ra8_nsc.h",
        "libs/ra8_nsc/src",
    );
    defer std.testing.allocator.free(line);
    try std.testing.expectEqualStrings(
        "  ra8_nsc_phantom: declared in libs/ra8_nsc/inc/ra8_nsc.h, no definition in libs/ra8_nsc/src/",
        line,
    );
}

test "the selftest fixtures declare one defined and one phantom veneer" {
    const names = try declared(implementation.selftest_header);
    defer release(names);
    try std.testing.expectEqual(@as(usize, 2), names.len);
    try std.testing.expectEqualStrings("ra8_nsc_defined", names[0]);
    try std.testing.expectEqualStrings("ra8_nsc_phantom", names[1]);
}

test "both selftest cases hold" {
    const cases = try implementation.selftestCases(std.testing.allocator);
    defer std.testing.allocator.free(cases);
    try std.testing.expectEqual(@as(usize, 2), cases.len);
    try std.testing.expect(cases[0].passed);
    try std.testing.expect(cases[1].passed);
    try std.testing.expectEqualStrings("matching veneer definition stays quiet", cases[0].label);
    try std.testing.expectEqualStrings("call-only phantom veneer fires", cases[1].label);
}

test "a non-ASCII letter glued to the name does not declare a veneer" {
    // Python's `\w` covers non-ASCII letters, so `\b` fails before the name
    // and the declaration scan finds nothing. Measured against the Python
    // this gate replaces: 0 declarations, exit 0.
    const names = try declared("RA8_NSC_VENEER ra8_err_t \u{e9}ra8_nsc_alpha(void);\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 0), names.len);
}

test "a non-ASCII-prefixed definition head does not define the declared veneer" {
    // The fail-open direction, and the reason `boundaryBefore` decodes: this
    // source defines a DIFFERENT function, so the declared veneer is still a
    // phantom. Measured: the Python reported it missing and exited 1.
    const source = "RA8_NSC_VENEER ra8_err_t \u{e9}ra8_nsc_alpha(void) { return 0; }\n";
    try std.testing.expect(!implementation.definesVeneer(source, "ra8_nsc_alpha"));
}

test "a no-break space before the name still declares and defines" {
    // The other side of the same boundary rule: a non-ASCII SPACE is in
    // Python's `\s`, so `\b` holds after it and the match stands. Narrowing
    // the boundary must not swallow this case.
    const names = try declared("RA8_NSC_VENEER ra8_err_t\u{a0}ra8_nsc_alpha(void);\n");
    defer release(names);
    try std.testing.expectEqual(@as(usize, 1), names.len);
    try std.testing.expectEqualStrings("ra8_nsc_alpha", names[0]);
    const source = "RA8_NSC_VENEER ra8_err_t\u{a0}ra8_nsc_alpha(void) { return 0; }\n";
    try std.testing.expect(implementation.definesVeneer(source, "ra8_nsc_alpha"));
}
