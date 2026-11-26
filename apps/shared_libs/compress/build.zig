//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `compress` facade
//! (RA8FW-1155). CMake consumes the installed static library through the
//! unchanged headers in inc/. Template: apps/shared_libs/jof/build.zig.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

fn addMinizLibrary(
    b: *std.Build,
    name: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
    extra_defines: []const []const u8,
) *std.Build.Step.Compile {
    const mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .sanitize_c = .off,
        .pic = true,
    });
    if (target.result.os.tag != .freestanding) mod.link_libc = true;
    var flags: std.ArrayList([]const u8) = .empty;
    flags.appendSlice(b.allocator, &.{
        "-std=gnu23",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-parameter",
        "-fno-strict-aliasing",
        "-DMINIZ_NO_STDIO",
        "-DMINIZ_NO_TIME",
    }) catch @panic("OOM");
    flags.appendSlice(b.allocator, extra_defines) catch @panic("OOM");
    mod.addCSourceFile(.{
        .file = b.path("../third_party/miniz/miniz.c"),
        .flags = flags.items,
    });
    mod.addIncludePath(b.path("../third_party/miniz"));
    const lib = b.addLibrary(.{
        .name = name,
        .linkage = .static,
        .root_module = mod,
    });
    lib.installHeader(b.path("../third_party/miniz/miniz.h"), "miniz.h");
    return lib;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const miniz_include = b.addNamedWriteFiles("miniz_include");
    _ = miniz_include.addCopyFile(b.path("../third_party/miniz/miniz.h"), "miniz.h");

    // Owned vendored miniz flavors (host/hosted builds consume via artifact()):
    // - "miniz": lean deflate/inflate without archive APIs or libc malloc
    // - "miniz_archive": reader build keeping zip archive APIs without malloc
    // - "miniz_host": host test build keeping malloc + archive APIs
    const miniz_lib = addMinizLibrary(b, "miniz", target, optimize, &.{ "-DMINIZ_NO_ARCHIVE_APIS", "-DMINIZ_NO_MALLOC" });
    const miniz_archive_lib = addMinizLibrary(b, "miniz_archive", target, optimize, &.{"-DMINIZ_NO_MALLOC"});
    const miniz_host_lib = addMinizLibrary(b, "miniz_host", target, optimize, &.{});
    if (target.result.os.tag != .freestanding) {
        b.installArtifact(miniz_lib);
        b.installArtifact(miniz_archive_lib);
        b.installArtifact(miniz_host_lib);
    }

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
    library.installHeader(b.path("inc/ra8_compress.h"), "ra8_compress.h");
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
    vfs_library.installHeader(b.path("inc/ra8_vfs_compress.h"), "ra8_vfs_compress.h");

    b.installArtifact(vfs_library);

    const vfs_test_under = b.createModule(.{
        .root_source_file = b.path("src/ra8_vfs_compress_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const vfs_test_module = b.createModule(.{
        .root_source_file = b.path("tests/vfs_compress_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    vfs_test_module.addImport("vfs_compress", vfs_test_under);
    vfs_test_module.addCSourceFile(.{ .file = b.path("teststub/vfs_compress_test_stubs.c"), .flags = &.{"-std=c23"} });
    const vfs_test = b.addTest(.{ .root_module = vfs_test_module });
    const run_vfs_test = b.addRunArtifact(vfs_test);
    const vfs_test_step = b.step("test", "Run package tests");
    vfs_test_step.dependOn(&run_vfs_test.step);
    vfs_test_step.dependOn(&miniz_lib.step);
    vfs_test_step.dependOn(&miniz_archive_lib.step);
    vfs_test_step.dependOn(&miniz_host_lib.step);
}
