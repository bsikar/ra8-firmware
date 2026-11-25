//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! tools/reader_vmem -- host build of the page-cache trace driver. The reader
//! drives the REAL ra8_vmem/ra8_vsource page cache over a book read and emits
//! the access trace cache_bench replays.
//!
//!   zig build --prefix build
//!   ./build/bin/reader_vmem build/reader_vmem.trace
//!
//! RA8FW-999: built through the Zig graph like every other host tool; the
//! C-era CMake build linked the same ra8_mem archive instead of compiling
//! ra8_vsource.c by path.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

// Every first-party .c in src/ is part of the one reader_vmem binary;
// reader_vmem.c defines main. The workspace test drives the planning layer
// through inc/reader_vmem_internal.h and excludes the driver front end.
const reader_sources = [_][]const u8{
    "reader_vmem.c",
    "reader_vmem_host.c",
    "reader_vmem_workspace.c",
};

const test_sources = [_][]const u8{
    "reader_vmem_host.c",
    "reader_vmem_workspace.c",
};

fn addSharedSources(b: *std.Build, mod: *std.Build.Module, files: []const []const u8) void {
    mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = files,
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror" },
    });
}

fn configureTargetModule(
    b: *std.Build,
    mod: *std.Build.Module,
    core_include_dir: std.Build.LazyPath,
    mem_lib: *std.Build.Step.Compile,
    files: []const []const u8,
) void {
    mod.link_libc = true;
    mod.linkLibrary(mem_lib);
    mod.addIncludePath(b.path("inc"));
    mod.addIncludePath(core_include_dir);
    addSharedSources(b, mod, files);
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    // The reader drives a real read, so the default build optimizes like the
    // C-era tool build did (-O2); an explicit -Doptimize still wins.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    const core_include_dir = b.dependency("ra8_core", .{ .target = target, .optimize = optimize }).artifact("ra8_core").getEmittedIncludeTree();
    const mem_dep = b.dependency("ra8_mem", .{ .target = target, .optimize = optimize });
    const mem_lib = mem_dep.artifact("ra8_mem");

    const mod = b.createModule(.{ .target = target, .optimize = optimize });
    configureTargetModule(b, mod, core_include_dir, mem_lib, &reader_sources);
    const exe = b.addExecutable(.{ .name = "reader_vmem", .root_module = mod });

    b.installArtifact(exe);

    const tmod = b.createModule(.{ .target = target, .optimize = optimize });
    configureTargetModule(b, tmod, core_include_dir, mem_lib, &test_sources);
    tmod.addCSourceFiles(.{
        .root = b.path("tests/src"),
        .files = &.{"reader_vmem_workspace_test.c"},
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror" },
    });
    const test_exe = b.addExecutable(.{ .name = "test_reader_vmem", .root_module = tmod });
    const run_test = b.addRunArtifact(test_exe);
    const test_step = b.step("test", "Run the reader_vmem workspace tests");
    test_step.dependOn(&run_test.step);

    const test_options = b.addOptions();
    test_options.addOptionPath("tool_exe", exe.getEmittedBin());
    const ztest_mod = b.createModule(.{
        .root_source_file = b.path("tests/reader_vmem_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    ztest_mod.addOptions("tool_options", test_options);
    const ztest = b.addTest(.{ .root_module = ztest_mod });
    const run_ztest = b.addRunArtifact(ztest);
    ra8_build.configureHostTestRun(b, test_step, ztest, run_ztest);
    test_step.dependOn(&run_ztest.step);
}
