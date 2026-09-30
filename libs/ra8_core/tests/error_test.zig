//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the error sink pair (#2875).
//!
//! The fatal path itself is not callable from a Zig test: it ends in a trap
//! by contract. `tests/misc/src/test_ra8_error_handler.c` covers that end,
//! catching the trap signal and longjmp-ing out. What is testable here is
//! everything the fatal path decides BEFORE it stops, plus the whole of the
//! non-fatal sink's substitution policy.

const std = @import("std");
const fatal = @import("error_fatal");
const sink = @import("error_sink");

// ---- fatal: what the halt sequence is on this build ---------------------

test "host builds are not on target" {
    try std.testing.expect(!fatal.on_target);
}

test "masking interrupts is a no-op off target rather than an illegal instruction" {
    fatal.maskInterrupts();
    fatal.maskInterrupts();
}

test "the breakpoint is a no-op off target rather than an illegal instruction" {
    fatal.breakpoint();
    fatal.breakpoint();
}

test "park never returns" {
    try std.testing.expectEqual(noreturn, @typeInfo(@TypeOf(fatal.park)).@"fn".return_type.?);
}

// ---- sink: the substitution policy --------------------------------------

test "a caller tag is passed through untouched" {
    const given: [*:0]const u8 = "I2C";
    try std.testing.expectEqual(given, sink.tagOr(given));
}

test "a null tag becomes the house tag" {
    try std.testing.expectEqualStrings(
        std.mem.span(sink.substitute.tag),
        std.mem.span(sink.tagOr(null)),
    );
}

test "a caller message is passed through untouched" {
    const given: [*:0]const u8 = "crc mismatch";
    try std.testing.expectEqual(given, sink.messageOr(given));
}

test "a null message becomes the house message" {
    try std.testing.expectEqualStrings(
        std.mem.span(sink.substitute.message),
        std.mem.span(sink.messageOr(null)),
    );
}

test "the substitutes are the strings the C suite asserts on the wire" {
    try std.testing.expectEqualStrings("ERR_SINK", std.mem.span(sink.substitute.tag));
    try std.testing.expectEqualStrings("(no message)", std.mem.span(sink.substitute.message));
}

test "an empty string is a caller value, not an omission" {
    const empty: [*:0]const u8 = "";
    try std.testing.expectEqual(empty, sink.tagOr(empty));
    try std.testing.expectEqual(empty, sink.messageOr(empty));
    try std.testing.expectEqual(@as(usize, 0), std.mem.span(sink.tagOr(empty)).len);
}
