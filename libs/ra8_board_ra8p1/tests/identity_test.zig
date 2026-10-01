//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Board identity: the three strings the BSP publishes.

const std = @import("std");

const Board = @import("identity").Board;

test "identity names the RA8P1 foundation board and its chip HUM" {
    try std.testing.expectEqualStrings("RA8P1 foundation board", Board.name);
    try std.testing.expectEqualStrings("R01UH1064EJ (chip HUM)", Board.doc_rev);
    try std.testing.expectEqualStrings("R7KA8P1KFLCAC", Board.mcu);
}
