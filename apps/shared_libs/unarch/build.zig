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

    // The xz container decoder over that seam, behind the unchanged
    // unarch_xz.h ABI. Still links the production budget seam from
    // ra8_core at the app level; the package test links
    // src/unarch_xz_teststub.zig for that seam and builds the real
    // vendored xz-embedded (decode-only) plus the real pool.
    const xz_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_xz_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const xz = b.addLibrary(.{
        .name = "unarch_xz",
        .linkage = .static,
        .root_module = xz_module,
    });
    xz.bundle_compiler_rt = xz.root_module.resolved_target.?.result.os.tag != .freestanding;
    xz.root_module.pic = true;
    b.installArtifact(xz);

    const xz_test_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_xz_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const xz_test = b.addTest(.{ .root_module = xz_test_module });
    xz_test.root_module.addCSourceFile(.{ .file = b.path("../third_party/xz_embedded/xz_crc32.c") });
    xz_test.root_module.addCSourceFile(.{ .file = b.path("../third_party/xz_embedded/xz_crc64.c") });
    xz_test.root_module.addCSourceFile(.{ .file = b.path("../third_party/xz_embedded/xz_dec_stream.c") });
    xz_test.root_module.addCSourceFile(.{ .file = b.path("../third_party/xz_embedded/xz_dec_lzma2.c") });
    xz_test.root_module.addCSourceFile(.{ .file = b.path("src/unarch_xz_pool.c"), .flags = &.{"-std=c23"} });
    xz_test.root_module.addCSourceFile(.{ .file = b.path("src/unarch_xz_fixture_stub.c"), .flags = &.{"-std=c23"} });
    xz_test.root_module.addIncludePath(b.path("inc"));
    xz_test.root_module.addIncludePath(b.path("../third_party/xz_embedded"));
    xz_test.root_module.addIncludePath(b.path("tests/inc"));
    xz_test.root_module.addIncludePath(b.path("../../../libs/ra8_core/inc"));
    xz_test.root_module.addIncludePath(b.path("../../../libs/ra8_imgdec/inc"));
    xz_test.root_module.addIncludePath(b.path("../../../libs/ra8_mem/inc"));
    xz_test.root_module.link_libc = true;

    const xz_stub_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_xz_teststub.zig"),
        .target = target,
        .optimize = optimize,
    });
    const xz_stub = b.addLibrary(.{
        .name = "unarch_xz_teststub",
        .linkage = .static,
        .root_module = xz_stub_module,
    });
    xz_test.root_module.linkLibrary(xz_stub);

    const run_xz_test = b.addRunArtifact(xz_test);

    const unarch_test_step = b.step("test", "Run package tests");
    unarch_test_step.dependOn(&run_io_test.step);
    unarch_test_step.dependOn(&run_tar_test.step);
    unarch_test_step.dependOn(&run_gzip_test.step);
    unarch_test_step.dependOn(&run_xz_test.step);
}
