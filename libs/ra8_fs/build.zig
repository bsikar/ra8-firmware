//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_fs`'s Zig half (RA8FW-724).
//!
//! ra8_fs is mid-port: the library lock is Zig, the rest of src/ is still C,
//! globbed into every app that names `ra8_fs` in LIBS. sources.cmake links this
//! archive beside that C because build.zig exists, so a unit joins the link by
//! moving here and deleting its .c. The headers in inc/ are unchanged.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});
    // No unwind tables in a freestanding archive, as in ra8_hal's (RA8FW-571).
    const unwind: ?std.builtin.UnwindTables = if (target.result.os.tag == .freestanding) .none else null;

    const root = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
        .unwind_tables = unwind,
    });
    // fs_c.zig @cImports the C headers so Zig units share their exact layouts.
    root.addIncludePath(b.path("inc"));
    root.addIncludePath(b.path("src"));
    root.addIncludePath(b.path("../ra8_core/inc"));
    const library = b.addLibrary(.{ .name = "ra8_fs", .linkage = .static, .root_module = root });
    library.link_function_sections = true;
    library.link_data_sections = true;
    library.bundle_compiler_rt = false;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_fs tests");
    const roots = [_][]const u8{
        "tests/lock_abi_test.zig",
        "tests/exfat_label_abi_test.zig",
        "tests/attr_abi_test.zig",
        "tests/utime_abi_test.zig",
        "tests/space_abi_test.zig",
        "tests/gpt_abi_test.zig",
        "tests/lfn_abi_test.zig",
        "tests/utf_abi_test.zig",
        "tests/upcase_abi_test.zig",
    };
    for (roots) |path| {
        const test_module = b.createModule(.{
            .root_source_file = b.path(path),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport("ra8_fs", root);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
