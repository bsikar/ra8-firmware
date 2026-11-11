//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the epub decompression guard:
//! src/epub_zip_guard.c (RA8FW-1160) behind the unchanged C headers in inc/.
//! CMake consumes the installed static library through those headers.
//! Template: apps/shared_libs/xml/build.zig (plain static library + host
//! test step). The epub core stays C and calls the two guard exports through
//! the archive link; the test step links the teststub C stand-ins for miniz
//! and the decompression policy.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/epub_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_epub",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig epub guard tests");
    const test_module = b.createModule(.{
        .root_source_file = b.path("src/epub_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addCSourceFile(.{ .file = b.path("teststub/zip_guard_test_stubs.c"), .flags = &.{"-std=c23"} });
    const epub_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(epub_test).step);
}
