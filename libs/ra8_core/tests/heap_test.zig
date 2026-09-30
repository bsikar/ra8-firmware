//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The heap policy the `_sbrk` trap reports (#2895).
//!
//! The trap body itself has nothing to test here: it is one ignored
//! parameter and an unconditional call into a `noreturn` sink, and the leg
//! that proves it never returns is a fork death test on the host
//! (`tests/hal/src/test_ra8_sbrk_trap_cov.c`), which needs a real process to
//! kill. What IS worth pinning is the three values that suite asserts by
//! hand, so an edit here fails in this file rather than in a C suite that
//! looks unrelated to it.

const std = @import("std");

const heap = @import("heap_sbrk");

test "the tag is the exact subsystem string the host suite matches" {
    try std.testing.expectEqualStrings("SBRK", heap.policy.tag);
}

test "the message names the policy it is enforcing" {
    try std.testing.expect(std.mem.indexOf(u8, heap.policy.message, "heap-free") != null);
    try std.testing.expect(std.mem.indexOf(u8, heap.policy.message, "_sbrk") != null);
}

test "no numeric code: the call itself is the fault" {
    try std.testing.expectEqual(@as(u32, 0), heap.policy.err);
}

test "both strings stay sentinel-terminated for the C sink" {
    try std.testing.expectEqual(@as(u8, 0), heap.policy.tag.ptr[heap.policy.tag.len]);
    try std.testing.expectEqual(@as(u8, 0), heap.policy.message.ptr[heap.policy.message.len]);
}
