//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const header_patch = @import("build_graph").middleware.header_patch;

const rewrites = [_]header_patch.Rewrite{
    .{ .old = "#define A 8", .new = "#define A 7" },
    .{ .old = "#define B 5", .new = "#define B 4" },
};

test "header_patch rewrites each matching line and keeps the rest" {
    const out = try header_patch.apply(std.testing.allocator, "/* x */\n#define A 8\n#define B 5\n", &rewrites);
    defer std.testing.allocator.free(out);
    try std.testing.expectEqualStrings("/* x */\n#define A 7\n#define B 4\n", out);
}

test "header_patch refuses a rewrite whose line is missing" {
    try std.testing.expectError(error.RewriteNotFound, header_patch.apply(std.testing.allocator, "#define A 8\n#define B  5\n", &rewrites));
}

test "header_patch refuses a rewrite whose line repeats" {
    try std.testing.expectError(error.RewriteNotUnique, header_patch.apply(std.testing.allocator, "#define A 8\n#define B 5\n#define A 8\n", &rewrites));
}

test "header_patch matches whole lines only" {
    try std.testing.expectEqual(@as(usize, 0), header_patch.count("#define A 80\n", "#define A 8"));
    try std.testing.expectEqual(@as(usize, 1), header_patch.count("x\n#define A 8", "#define A 8"));
}
