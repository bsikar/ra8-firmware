//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `unarch` read seam
//! (RA8FW-1196). CMake consumes the installed static library through the
//! unchanged headers in inc/. Template: apps/shared_libs/compress/build.zig.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_io_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "unarch_io",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_io_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const unarch_test = b.addTest(.{ .root_module = test_module });
    const run_unarch_test = b.addRunArtifact(unarch_test);
    const unarch_test_step = b.step("test", "Run package tests");
    unarch_test_step.dependOn(&run_unarch_test.step);
}
