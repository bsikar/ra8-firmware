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
