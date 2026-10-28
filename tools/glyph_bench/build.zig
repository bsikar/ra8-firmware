//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! tools/glyph_bench -- host build of the glyph-cache budget sweep. The sweep
//! drives the REAL ra8_glyph_atlas over a realistic page-render glyph stream
//! and reports hit rate and rasterisations saved per cell budget.
//!
//!   zig build --prefix build
//!   ./build/bin/glyph_bench
//!
//! RA8FW-999: built through the Zig graph like every other host tool; the
//! C-era CMake build linked the same three Zig archives (ra8_io, ra8_mem,
//! ra8_core's ra8_core_zig) instead of compiling their sources by path.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    // The benchmark measures, so the default build optimizes like the C-era
    // tool build did (-O2); an explicit -Doptimize still wins.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    // ra8_log and the glyph atlas are Zig now, so the bench links the ra8_core,
    // ra8_mem and ra8_io archives instead of compiling those sources by path.
    // ra8_io is built with function sections, so the linker drops the
    // block-device units the bench never reaches (--gc-sections below).
    // The bench reaches only the stream ABI, so it links ra8_io's narrow
    // stream archive: root.zig's comptime block would keep every facade (and
    // its extern driver deps) in the archive, which --gc-sections cannot drop.
    const io_dep = b.dependency("ra8_io", .{ .target = target, .optimize = optimize, .units = "stream" });
    const mem_dep = b.dependency("ra8_mem", .{ .target = target, .optimize = optimize });
    const core_dep = b.dependency("ra8_core", .{ .target = target, .optimize = optimize });

    const mod = b.createModule(.{ .target = target, .optimize = optimize });
    mod.link_libc = true;
    mod.linkLibrary(io_dep.artifact("ra8_io"));
    mod.linkLibrary(mem_dep.artifact("ra8_mem"));
    mod.linkLibrary(core_dep.artifact("ra8_core_zig"));
    mod.addIncludePath(b.path("../../libs/ra8_core/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_mem/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_io/inc"));
    mod.addIncludePath(b.path("../../port/posix/inc"));
    mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{"glyph_bench.c"},
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-DRA8_OFF_TARGET" },
    });
    mod.addCSourceFiles(.{
        .root = b.path("../../port/posix/src"),
        .files = &.{"ra8_io_stream_posix.c"},
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-DRA8_OFF_TARGET" },
    });

    const exe = b.addExecutable(.{ .name = "glyph_bench", .root_module = mod });
    exe.link_gc_sections = true;
    b.installArtifact(exe);
}
