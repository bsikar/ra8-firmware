//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const alias = @import("periph_alias");

test "a Secure image keeps the Secure base" {
    try std.testing.expectEqual(@as(usize, 0x40203000), alias.of(0x40203000, false));
    try std.testing.expectEqual(@as(usize, 0x40250000), alias.of(0x40250000, false));
}

test "a Non-secure image sets IDAU bit 28" {
    try std.testing.expectEqual(@as(usize, 0x50203000), alias.of(0x40203000, true));
    try std.testing.expectEqual(@as(usize, 0x50250000), alias.of(0x40250000, true));
    try std.testing.expectEqual(@as(usize, 0x50351000), alias.of(0x40351000, true));
}
