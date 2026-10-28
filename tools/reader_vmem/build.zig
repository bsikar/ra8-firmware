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

fn addSharedIncludes(b: *std.Build, mod: *std.Build.Module) void {
    mod.addIncludePath(b.path("inc"));
    mod.addIncludePath(b.path("../../libs/ra8_core/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_mem/inc"));
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    // The reader drives a real read, so the default build optimizes like the
    // C-era tool build did (-O2); an explicit -Doptimize still wins.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    // ra8_vsource is Zig now, so the reader links the ra8_mem archive instead
    // of compiling that file by path. The archive brings along every extern in
    // it (ra8_vmem_get/ra8_vmem_put from the ported stream adapter, the four
    // ra8_keycache_* from the glyph atlas); both C halves are already in these
    // targets, so they resolve; the workspace test links the same archive.
    const mem_dep = b.dependency("ra8_mem", .{ .target = target, .optimize = optimize });
    const mem_lib = mem_dep.artifact("ra8_mem");

    const mod = b.createModule(.{ .target = target, .optimize = optimize });
    mod.link_libc = true;
    mod.linkLibrary(mem_lib);
    addSharedIncludes(b, mod);
    addSharedSources(b, mod, &reader_sources);
    const exe = b.addExecutable(.{ .name = "reader_vmem", .root_module = mod });

    b.installArtifact(exe);

    const tmod = b.createModule(.{ .target = target, .optimize = optimize });
    tmod.link_libc = true;
    tmod.linkLibrary(mem_lib);
    addSharedIncludes(b, tmod);
    addSharedSources(b, tmod, &test_sources);
    tmod.addCSourceFiles(.{
        .root = b.path("tests/src"),
        .files = &.{"reader_vmem_workspace_test.c"},
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror" },
    });
    const test_exe = b.addExecutable(.{ .name = "test_reader_vmem", .root_module = tmod });
    const run_test = b.addRunArtifact(test_exe);
    const test_step = b.step("test", "Run the reader_vmem workspace tests");
    test_step.dependOn(&run_test.step);
}
