//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the root build graph's own rules (#857).
//!
//! The graph encodes parity facts that a directory listing cannot tell you:
//! which board translation units are opt-in rather than universal, which
//! warning classes the vendored decoder may suppress and which it may not, and
//! how a compile command is escaped into the database the analysis gates parse.
//! Each of those is ordinary data or an ordinary function, so it is tested here
//! directly instead of only being exercised the long way round through a build.

const std = @import("std");
const graph = @import("build_graph");
const abi = graph.abi_contract;

test "board opt-in gate drops the two sources an app must ask for" {
    try std.testing.expect(
        graph.isGatedOutBoardSource("libs/ra8_board_ek_ra8d2/src/ra8_board_console_stream.c"),
    );
    try std.testing.expect(
        graph.isGatedOutBoardSource("libs/ra8_board_ek_ra8d2/src/ra8_board_touch.c"),
    );
}

test "board opt-in gate keeps the universal board sources" {
    try std.testing.expect(
        !graph.isGatedOutBoardSource("libs/ra8_board_ek_ra8d2/src/ra8_board_clock.c"),
    );
    try std.testing.expect(
        !graph.isGatedOutBoardSource("libs/ra8_board_ek_ra8d2/src/ra8_board_pins.c"),
    );
    // The gate matches a suffix, not a substring: a source that merely mentions
    // the gated name must still be compiled.
    try std.testing.expect(
        !graph.isGatedOutBoardSource("libs/ra8_board_ek_ra8d2/src/ra8_board_touch_probe.c"),
    );
}

test "host C bar keeps -Werror and both off-target definitions" {
    var has_werror = false;
    var has_off_target = false;
    var has_unit_test = false;
    for (graph.c_flags) |flag| {
        if (std.mem.eql(u8, flag, "-Werror")) has_werror = true;
        if (std.mem.eql(u8, flag, "-DRA8_OFF_TARGET")) has_off_target = true;
        if (std.mem.eql(u8, flag, "-DUNIT_TEST")) has_unit_test = true;
    }
    try std.testing.expect(has_werror);
    try std.testing.expect(has_off_target);
    try std.testing.expect(has_unit_test);
}

test "vendored suppression is narrow and ordered" {
    // -Wno-conversion has to come AFTER the -Wconversion the first-party bar
    // turns on, or the suppression suppresses nothing and the SOUP parity claim
    // is empty. Order is the whole content of the rule, so assert on it.
    var enabled_at: ?usize = null;
    var suppressed_at: ?usize = null;
    for (graph.vendored_soup_flags, 0..) |flag, index| {
        if (std.mem.eql(u8, flag, "-Wconversion")) enabled_at = index;
        if (std.mem.eql(u8, flag, "-Wno-conversion")) suppressed_at = index;
    }
    try std.testing.expect(enabled_at != null);
    try std.testing.expect(suppressed_at != null);
    try std.testing.expect(enabled_at.? < suppressed_at.?);

    // Everything else stays at the first-party bar on an attacker-facing
    // decoder: no blanket -w, and -Werror survives.
    for (graph.vendored_soup_flags) |flag| {
        try std.testing.expect(!std.mem.eql(u8, flag, "-w"));
    }
    var has_werror = false;
    for (graph.vendored_soup_flags) |flag| {
        if (std.mem.eql(u8, flag, "-Werror")) has_werror = true;
    }
    try std.testing.expect(has_werror);
}

test "compile database escapes the bytes JSON cannot carry raw" {
    var out = std.ArrayList(u8).init(std.testing.allocator);
    defer out.deinit();
    graph.appendJsonString(&out, "a\"b\\c\nd\te");
    try std.testing.expectEqualStrings("\"a\\\"b\\\\c\\nd\\te\"", out.items);
}

test "compile database leaves an ordinary path untouched" {
    var out = std.ArrayList(u8).init(std.testing.allocator);
    defer out.deinit();
    graph.appendJsonString(&out, "libs/ra8_core/src/ra8_log.c");
    try std.testing.expectEqualStrings("\"libs/ra8_core/src/ra8_log.c\"", out.items);
}

test "ABI negative control passes only on a failure that names its reason" {
    const layout = abi.negative_fixtures[0];
    try std.testing.expectEqual(abi.NegativeKind.layout, layout.kind);

    // The whole point of a negative control: a compile that SUCCEEDS is the
    // failure, not the pass.
    try std.testing.expectEqual(
        abi.NegativeOutcome.unexpected_success,
        abi.classifyNegative(false, layout.expected_diagnostic, layout.expected_diagnostic),
    );
    // And a failure for an unrelated reason -- a missing header, a mistyped
    // flag -- proves nothing about the boundary, so it is not a pass either.
    try std.testing.expectEqual(
        abi.NegativeOutcome.missing_diagnostic,
        abi.classifyNegative(true, "fatal error: 'ra8_err.h' file not found", layout.expected_diagnostic),
    );
    try std.testing.expectEqual(
        abi.NegativeOutcome.expected_failure,
        abi.classifyNegative(
            true,
            "negative_layout.c:11:1: error: static assertion failed: \"ABI contract fixture deliberately requires an incompatible layout\"",
            layout.expected_diagnostic,
        ),
    );
}

test "ABI layout control stops before the link, the symbol control does not" {
    const allocator = std.testing.allocator;
    for (abi.negative_fixtures) |fixture| {
        const arguments = abi.negativeArguments(allocator, "zig", fixture, "/tmp/libfixture.a", "/tmp/out");
        defer allocator.free(arguments);

        var has_dialect = false;
        var links_archive = false;
        var compile_only = false;
        for (arguments) |argument| {
            if (std.mem.eql(u8, argument, abi.negative_dialect_flag)) has_dialect = true;
            if (std.mem.eql(u8, argument, "/tmp/libfixture.a")) links_archive = true;
            if (std.mem.eql(u8, argument, "-c")) compile_only = true;
        }
        try std.testing.expect(has_dialect);
        switch (fixture.kind) {
            // A missing export is only missing at the link, and only against
            // the real archive.
            .missing_symbol => {
                try std.testing.expect(links_archive);
                try std.testing.expect(!compile_only);
            },
            // Layout drift is a compile-time assertion; linking it would only
            // add an unrelated undefined `main` to the diagnostics.
            .layout => {
                try std.testing.expect(!links_archive);
                try std.testing.expect(compile_only);
            },
        }
    }
}

test "ABI consumer keeps the warning bar CMake puts on its consumer" {
    var has_werror = false;
    for (abi.consumer_flags) |flag| {
        if (std.mem.eql(u8, flag, "-Werror")) has_werror = true;
        try std.testing.expect(!std.mem.eql(u8, flag, "-w"));
    }
    try std.testing.expect(has_werror);
    // The fixture's own public header and the shared error vocabulary, in the
    // order zig_abi_contract.cmake puts them on the include path.
    try std.testing.expectEqualStrings("tests/zig_abi_fixture/inc", abi.include_paths[0]);
    try std.testing.expectEqualStrings("libs/ra8_core/inc", abi.include_paths[1]);
}
