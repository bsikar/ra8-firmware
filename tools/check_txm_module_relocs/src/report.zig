//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What the checker says: one line for each finding, then a verdict.

const std = @import("std");
const arm = @import("arm.zig");
const check = @import("check.zig");

/// Check `image` and write the report for the file called `path`. Returns
/// how many findings there were; zero is a pass.
pub fn write(writer: anytype, path: []const u8, image: []const u8) !usize {
    var findings = try check.Iterator.init(image);
    var count: usize = 0;
    while (try findings.next()) |finding| : (count += 1) {
        try line(writer, path, finding);
    }
    if (count == 0) {
        try writer.print("{s}: OK, no absolute relocation in a data section\n", .{path});
    } else {
        try writer.print(
            "{s}: FAIL, {d} word(s) of data hold an address nothing rebases at load\n",
            .{ path, count },
        );
    }
    return count;
}

fn line(writer: anytype, path: []const u8, finding: check.Finding) !void {
    try writer.print("{s}: {s} at 0x{x:0>8}: ", .{ path, finding.section, finding.address });
    if (arm.name(finding.kind)) |name| {
        try writer.writeAll(name);
    } else {
        try writer.print("relocation type {d}", .{finding.kind});
    }
    try writer.print(" -> {s} (0x{x:0>8})\n", .{ finding.symbol, finding.value });
}
