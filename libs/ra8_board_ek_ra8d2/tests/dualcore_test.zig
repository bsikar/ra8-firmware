//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The shared-window descriptor. No seam: the module answers from constants
//! and the build flag, which is the whole point of it.

const std = @import("std");
const dualcore = @import("dualcore");

test "a null out is refused" {
    try std.testing.expectEqual(dualcore.Err.null_ptr, dualcore.describe(null));
}

test "the window starts at SRAM2" {
    var out: dualcore.SharedRam = undefined;
    try std.testing.expectEqual(dualcore.Err.ok, dualcore.describe(&out));
    try std.testing.expectEqual(@as(usize, 0x2210_0000), @intFromPtr(out.base.?));
}

test "the window is 576 KiB" {
    var out: dualcore.SharedRam = undefined;
    _ = dualcore.describe(&out);
    try std.testing.expectEqual(@as(u32, 0x9_0000), out.size_bytes);
    try std.testing.expectEqual(@as(u32, 576 * 1024), out.size_bytes);
}

test "the window ends where CPU1's private bank begins" {
    var out: dualcore.SharedRam = undefined;
    _ = dualcore.describe(&out);
    const end = @intFromPtr(out.base.?) + out.size_bytes;
    try std.testing.expectEqual(@as(usize, 0x2219_0000), end);
}

test "without the boot cache MPU the descriptor promises nothing" {
    var out: dualcore.SharedRam = undefined;
    _ = dualcore.describe(&out);
    try std.testing.expectEqual(dualcore.non_cacheable, out.non_cacheable);
    try std.testing.expectEqual(false, out.non_cacheable);
}
