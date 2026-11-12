//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `compress` facade
//! (RA8FW-1155). CMake consumes the installed static library through the
//! unchanged headers in inc/. Template: apps/shared_libs/jof/build.zig.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_compress_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "compress",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const vfs_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_vfs_compress_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const vfs_library = b.addLibrary(.{
        .name = "vfs_compress",
        .linkage = .static,
        .root_module = vfs_module,
    });
    vfs_library.bundle_compiler_rt = vfs_library.root_module.resolved_target.?.result.os.tag != .freestanding;
    vfs_library.root_module.pic = true;

        b.installArtifact(vfs_library);

    const vfs_test_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_vfs_compress_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    vfs_test_module.addCSourceFile(.{ .file = b.path("teststub/vfs_compress_test_stubs.c"), .flags = &.{"-std=c23"} });
    const vfs_test = b.addTest(.{ .root_module = vfs_test_module });
    const run_vfs_test = b.addRunArtifact(vfs_test);
    const vfs_test_step = b.step("test", "Run package tests");
    vfs_test_step.dependOn(&run_vfs_test.step);

}
