//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three identity strings are read by name from C, so the values matter
//! as much as the fill path.

const std = @import("std");
const board_info = @import("board_info");

test "identity strings match the UM cover page" {
    try std.testing.expectEqualStrings("EK-RA8D2 v1", board_info.name);
    try std.testing.expectEqualStrings("R20UT5523EG0101 Rev 1.01", board_info.doc_rev);
    try std.testing.expectEqualStrings("R7KA8D2KFLCAC", board_info.mcu);
}

test "fill copies all three and reports ok" {
    var out: board_info.Info = undefined;
    try std.testing.expectEqual(@as(u32, 0), board_info.fill(&out));
    try std.testing.expectEqualStrings("EK-RA8D2 v1", std.mem.span(out.name));
    try std.testing.expectEqualStrings("R20UT5523EG0101 Rev 1.01", std.mem.span(out.doc_rev));
    try std.testing.expectEqualStrings("R7KA8D2KFLCAC", std.mem.span(out.mcu));
}

test "fill hands out the same pointers the C constants publish" {
    var out: board_info.Info = undefined;
    _ = board_info.fill(&out);
    try std.testing.expectEqual(board_info.name.ptr, out.name);
}

test "strings are sentinel terminated for the C ABI" {
    try std.testing.expectEqual(@as(u8, 0), board_info.name[board_info.name.len]);
    try std.testing.expectEqual(@as(u8, 0), board_info.doc_rev[board_info.doc_rev.len]);
    try std.testing.expectEqual(@as(u8, 0), board_info.mcu[board_info.mcu.len]);
}
