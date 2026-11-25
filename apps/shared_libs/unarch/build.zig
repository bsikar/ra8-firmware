//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `unarch` read seam
//! (RA8FW-1196) and tar walker (RA8FW-1197). CMake consumes the installed
//! static libraries through the unchanged headers in inc/. Template:
//! apps/shared_libs/compress/build.zig.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const core_lib = b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_core");
    const imgdec_lib = b.dependency("ra8_imgdec", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_imgdec");
    const mem_lib = b.dependency("ra8_mem", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_mem");
    const compress_dep = b.dependency("compress", .{
        .target = target,
        .optimize = optimize,
    });

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
    io.installHeader(b.path("inc/unarch_io.h"), "unarch_io.h");
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
    tar.installHeader(b.path("inc/unarch_tar.h"), "unarch_tar.h");
    b.installArtifact(tar);

    // The gzip member decoder over that seam, behind the unchanged
    // unarch_gzip.h ABI. Still links the production budget seam from
    // ra8_core at the app level; the package test links
    // src/unarch_gzip_teststub.zig for that seam and compress's miniz_host
    // artifact for the tinfl one.
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
    gzip.installHeader(b.path("inc/unarch_gzip.h"), "unarch_gzip.h");
    b.installArtifact(gzip);

    const io_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/unarch_io_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const io_test_module = b.createModule(.{
        .root_source_file = b.path("tests/unarch_io_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    io_test_module.addImport("unarch_io_abi", io_test_module_sut);
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
    const gzip_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/unarch_gzip_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const gzip_test_module = b.createModule(.{
        .root_source_file = b.path("tests/unarch_gzip_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    gzip_test_module.addImport("unarch_gzip_abi", gzip_test_module_sut);
    const gzip_test = b.addTest(.{ .root_module = gzip_test_module });
    if (target.result.os.tag != .freestanding) {
        gzip_test.root_module.linkLibrary(compress_dep.artifact("miniz_host"));
        gzip_test.root_module.link_libc = true;
    }

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
    xz.installHeader(b.path("inc/unarch_xz.h"), "unarch_xz.h");

    // Owned vendored xz-embedded decode-only core behind xz_config.h.
    const xz_vendor_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    if (target.result.os.tag != .freestanding) xz_vendor_mod.link_libc = true;
    xz_vendor_mod.addIncludePath(b.path("../third_party/xz_embedded"));
    xz_vendor_mod.addIncludePath(b.path("inc"));
    xz_vendor_mod.addIncludePath(imgdec_lib.getEmittedIncludeTree());
    xz_vendor_mod.addIncludePath(mem_lib.getEmittedIncludeTree());
    xz_vendor_mod.addIncludePath(core_lib.getEmittedIncludeTree());
    var xz_flags: std.ArrayList([]const u8) = .empty;
    xz_flags.appendSlice(b.allocator, &.{
        "-std=c23",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wconversion",
        "-Wno-conversion",
        "-fno-strict-aliasing",
    }) catch @panic("OOM");
    if (target.result.os.tag == .windows or target.result.os.tag == .macos) {
        xz_flags.append(b.allocator, "-D__always_inline=inline") catch @panic("OOM");
    }
    xz_vendor_mod.addCSourceFiles(.{
        .root = b.path("../third_party/xz_embedded"),
        .files = &.{
            "xz_crc32.c",
            "xz_crc64.c",
            "xz_dec_stream.c",
            "xz_dec_lzma2.c",
        },
        .flags = xz_flags.items,
    });
    const xz_vendor = b.addLibrary(.{
        .name = "xz_embedded",
        .linkage = .static,
        .root_module = xz_vendor_mod,
    });
    xz_vendor.installHeadersDirectory(b.path("../third_party/xz_embedded"), "", .{});
    b.installArtifact(xz);
    if (target.result.os.tag != .freestanding) b.installArtifact(xz_vendor);

    const xz_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/unarch_xz_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const xz_test_module = b.createModule(.{
        .root_source_file = b.path("tests/unarch_xz_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    xz_test_module.addImport("unarch_xz_abi", xz_test_module_sut);
    const xz_test = b.addTest(.{ .root_module = xz_test_module });
    // The zero-heap XZ allocation pool, ported to Zig behind the
    // unchanged unarch_xz_pool.h ABI (RA8FW-1202); the package test links
    // it the same way the app build does.
    const pool_module = b.createModule(.{
        .root_source_file = b.path("src/unarch_xz_pool_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const pool = b.addLibrary(.{
        .name = "unarch_xz_pool",
        .linkage = .static,
        .root_module = pool_module,
    });
    pool.bundle_compiler_rt = pool.root_module.resolved_target.?.result.os.tag != .freestanding;
    pool.installHeader(b.path("inc/unarch_xz_pool.h"), "unarch_xz_pool.h");
    pool.installHeader(b.path("inc/xz_config.h"), "xz_config.h");
    b.installArtifact(pool);
    xz_test.root_module.linkLibrary(xz_vendor);
    xz_test.root_module.linkLibrary(pool);
    xz_test.root_module.addCSourceFile(.{ .file = b.path("src/unarch_xz_fixture_stub.c"), .flags = &.{"-std=c23"} });
    xz_test.root_module.addIncludePath(imgdec_lib.getEmittedIncludeTree());
    xz_test.root_module.addIncludePath(mem_lib.getEmittedIncludeTree());
    xz_test.root_module.addIncludePath(core_lib.getEmittedIncludeTree());
    xz_test.root_module.addIncludePath(b.path("tests/inc"));
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
