//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `unarch` read seam
//! (RA8FW-1196) and tar walker (RA8FW-1197). CMake consumes the installed
//! static libraries through the unchanged headers in inc/. Template:
//! apps/shared_libs/compress/build.zig.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // The read seam: flat-memory reader behind the unchanged unarch_io.h ABI.
    const io_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_io_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const io = b.addLibrary(.{
        .name = "unarch_io",
        .linkage = .static,
        .root_module = io_module,
    });
    io.bundle_compiler_rt = io.root_module.resolved_target.?.result.os.tag != .freestanding;
    io.root_module.pic = true;
    b.installArtifact(io);

    // The tar walker over that seam, behind the unchanged unarch_tar.h ABI.
    // Still links the production budget seam from ra8_core at the app level;
    // the package test links src/unarch_tar_teststub.zig instead.
    const tar_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_tar_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const tar = b.addLibrary(.{
        .name = "unarch_tar",
        .linkage = .static,
        .root_module = tar_module,
    });
    tar.bundle_compiler_rt = tar.root_module.resolved_target.?.result.os.tag != .freestanding;
    tar.root_module.pic = true;
    b.installArtifact(tar);

    // The gzip member decoder over that seam, behind the unchanged
    // unarch_gzip.h ABI. Still links the production budget seam from
    // ra8_core at the app level; the package test links
    // src/unarch_gzip_teststub.zig for that seam and the real vendored
    // miniz for the tinfl one.
    const gzip_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_gzip_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const gzip = b.addLibrary(.{
        .name = "unarch_gzip",
        .linkage = .static,
        .root_module = gzip_module,
    });
    gzip.bundle_compiler_rt = gzip.root_module.resolved_target.?.result.os.tag != .freestanding;
    gzip.root_module.pic = true;
    b.installArtifact(gzip);

    const io_test_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_io_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const io_test = b.addTest(.{ .root_module = io_test_module });
    const run_io_test = b.addRunArtifact(io_test);

    const tar_test_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_tar_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const tar_test = b.addTest(.{ .root_module = tar_test_module });

    const tar_stub_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_tar_teststub.zig"),
        .target = target,
        .optimize = optimize,
    });
    const tar_stub = b.addLibrary(.{
        .name = "unarch_tar_teststub",
        .linkage = .static,
        .root_module = tar_stub_module,
    });
    tar_test.root_module.linkLibrary(tar_stub);

    const run_tar_test = b.addRunArtifact(tar_test);
    const gzip_test_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_gzip_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const gzip_test = b.addTest(.{ .root_module = gzip_test_module });
    gzip_test.root_module.addCSourceFile(.{ .file = b.path("../third_party/miniz/miniz.c") });
    gzip_test.root_module.addIncludePath(b.path("../third_party/miniz"));
    gzip_test.root_module.link_libc = true;

    const gzip_stub_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_gzip_teststub.zig"),
        .target = target,
        .optimize = optimize,
    });
    const gzip_stub = b.addLibrary(.{
        .name = "unarch_gzip_teststub",
        .linkage = .static,
        .root_module = gzip_stub_module,
    });
    gzip_test.root_module.linkLibrary(gzip_stub);

    const run_gzip_test = b.addRunArtifact(gzip_test);

    const unarch_test_step = b.step("test", "Run package tests");
    unarch_test_step.dependOn(&run_io_test.step);
    unarch_test_step.dependOn(&run_tar_test.step);
    unarch_test_step.dependOn(&run_gzip_test.step);
}
