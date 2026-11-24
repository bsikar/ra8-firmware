//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for Windows PATH sanitization and tool lookup in cross_build.zig.

const std = @import("std");
const graph = @import("build_graph");
const cb = graph.cross_build;

test "Windows PATH sanitization rejects malformed entries and preserves valid ones" {
    try std.testing.expectEqualStrings(
        "C:\\WINDOWS\\system32",
        cb.sanitizeWindowsPathEntry("C:\\WINDOWS\\system32").?,
    );
    try std.testing.expectEqualStrings(
        "C:\\Program Files (x86)\\Arm GNU Toolchain\\bin",
        cb.sanitizeWindowsPathEntry("\"C:\\Program Files (x86)\\Arm GNU Toolchain\\bin\"").?,
    );
    try std.testing.expectEqualStrings(
        "\\\\?\\C:\\tools\\bin",
        cb.sanitizeWindowsPathEntry("\\\\?\\C:\\tools\\bin").?,
    );
    try std.testing.expectEqualStrings(
        "\\\\?\\UNC\\server\\share\\bin",
        cb.sanitizeWindowsPathEntry("\\\\?\\UNC\\server\\share\\bin").?,
    );
    try std.testing.expect(cb.sanitizeWindowsPathEntry("") == null);
    try std.testing.expect(cb.sanitizeWindowsPathEntry("\"\"") == null);
    try std.testing.expect(cb.sanitizeWindowsPathEntry(
        "C:\\ProgramData\\GCC for Renesas RX\\binC:\\Program Files\\Common Files\\Oracle\\Java\\javapath",
    ) == null);
    try std.testing.expect(cb.sanitizeWindowsPathEntry("C:\\bad\"quote\\bin") == null);
    try std.testing.expect(cb.sanitizeWindowsPathEntry("C:\\bad*wild\\bin") == null);
    try std.testing.expect(cb.sanitizeWindowsPathEntry("C:\\bad?q\\bin") == null);
    try std.testing.expect(cb.sanitizeWindowsPathEntry("\\\\?") == null);
    try std.testing.expect(cb.sanitizeWindowsPathEntry("\\\\") == null);
}

test "findExecutableInPath skips malformed PATH entries and finds tool in later dir" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try tmp.dir.createDirPath(std.testing.io, "sub_dir");
    try tmp.dir.createDirPath(std.testing.io, "sub_dir/arm-none-eabi-gcc");
    {
        var f = try tmp.dir.createFile(std.testing.io, "sub_dir/arm-none-eabi-gcc.exe", .{});
        f.close(std.testing.io);
    }

    const abs_root = try tmp.dir.realPathFileAlloc(std.testing.io, ".", std.testing.allocator);
    defer std.testing.allocator.free(abs_root);
    const bin_dir = try std.fs.path.join(std.testing.allocator, &.{ abs_root, "sub_dir" });
    defer std.testing.allocator.free(bin_dir);

    // Malformed concatenated entry first, then nonexistent dir, then real dir.
    // Note: sub_dir contains a subdirectory named `arm-none-eabi-gcc`, which must
    // be ignored in favor of `arm-none-eabi-gcc.exe`.
    const path_env = try std.fmt.allocPrint(
        std.testing.allocator,
        "C:\\bad\\binC:\\other;C:\\missing\\dir;{s}",
        .{bin_dir},
    );
    defer std.testing.allocator.free(path_env);

    const found = cb.findExecutableInPath(
        std.testing.allocator,
        std.testing.io,
        path_env,
        ';',
        "arm-none-eabi-gcc",
        ".com;.exe",
    ).?;
    defer std.testing.allocator.free(found);

    const expected = try std.fs.path.join(std.testing.allocator, &.{ bin_dir, "arm-none-eabi-gcc.exe" });
    defer std.testing.allocator.free(expected);
    try std.testing.expectEqualStrings(expected, found);
}
