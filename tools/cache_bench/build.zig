//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! tools/cache_bench -- host build of the eviction benchmark and the
//! block/frame-size sweep. The sweep drives the REAL ra8_vmem/ra8_vsource
//! page cache and the REAL book_chunked RBKC reader.
//!
//!   zig build --prefix build
//!   ./build/bin/cache_bench --sweep-block
//!
//! RA8FW-999: built through the Zig graph like every other host tool; the
//! C-era CMake build compiled the same source set with the same warning bar
//! (miniz, SOUP, kept its -fno-strict-aliasing optimizer contract, which
//! guards against strict-aliasing miscompilation of its inflate at -O2).

const std = @import("std");

// Every first-party .c in src/ is part of the one cache_bench binary
// (mirrors the authoritative tools recipe's source set), so new TUs need no
// edit here or there. cache_bench_report.c is the command-line front end
// (it defines main); the contract test drives the replay engine through
// inc/cache_bench.h instead and excludes it.
const bench_sources = [_][]const u8{
    "cache_bench.c",
    "cache_bench_host.c",
    "cache_bench_io.c",
    "cache_bench_report.c",
    "policies.c",
    "policy_scanresist.c",
    "sweep_block.c",
    "sweep_block_backends.c",
    "sweep_block_payload.c",
    "sweep_block_report.c",
    "trace.c",
};

const book_tus = [_][]const u8{
    "book.c",
    "book_chunked.c",
};

// The contract test drives the replay engine through inc/cache_bench.h:
// same first-party set as the tool minus the command-line front end
// (cache_bench_report.c defines main).
const test_sources = [_][]const u8{
    "cache_bench.c",
    "cache_bench_host.c",
    "cache_bench_io.c",
    "policies.c",
    "policy_scanresist.c",
    "sweep_block.c",
    "sweep_block_backends.c",
    "sweep_block_payload.c",
    "sweep_block_report.c",
    "trace.c",
};

fn addSharedSources(b: *std.Build, mod: *std.Build.Module, files: []const []const u8) void {
    mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = files,
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-DMINIZ_NO_MALLOC", "-DMINIZ_NO_STDIO" },
    });
    mod.addCSourceFiles(.{
        .root = b.path("../../apps/shared_libs/book/src"),
        .files = &book_tus,
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-DMINIZ_NO_MALLOC", "-DMINIZ_NO_STDIO" },
    });
    // miniz is SOUP and type-puns through byte buffers; keep the optimizer
    // contract that prevents strict-aliasing miscompilation.
    mod.addCSourceFiles(.{
        .root = b.path("../../apps/shared_libs/third_party/miniz"),
        .files = &.{"miniz.c"},
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-fno-strict-aliasing", "-DMINIZ_NO_MALLOC", "-DMINIZ_NO_STDIO" },
    });
}

fn addSharedIncludes(b: *std.Build, mod: *std.Build.Module) void {
    mod.addIncludePath(b.path("inc"));
    mod.addIncludePath(b.path("../../libs/ra8_core/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_mem/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/book/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/third_party/miniz"));
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    // The benchmark measures, so the default build optimizes like the C-era
    // tool build did (-O2); an explicit -Doptimize still wins.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    // ra8_vsource is Zig now, so the bench links the ra8_mem archive instead
    // of compiling that file by path. The archive is a single object, so it
    // brings along every extern in it (ra8_vmem_get/ra8_vmem_put from the
    // ported stream adapter, the four ra8_keycache_* from the glyph atlas);
    // both C halves are already in these targets, so they resolve; the contract test links the same archive.
    const mem_dep = b.dependency("ra8_mem", .{ .target = target, .optimize = optimize });
    const mem_lib = mem_dep.artifact("ra8_mem");

    const mod = b.createModule(.{ .target = target, .optimize = optimize });
    mod.link_libc = true;
    mod.linkLibrary(mem_lib);
    addSharedIncludes(b, mod);
    addSharedSources(b, mod, &bench_sources);
    const exe = b.addExecutable(.{ .name = "cache_bench", .root_module = mod });

    b.installArtifact(exe);

    const tmod = b.createModule(.{ .target = target, .optimize = optimize });
    tmod.link_libc = true;
    tmod.linkLibrary(mem_lib);
    addSharedIncludes(b, tmod);
    addSharedSources(b, tmod, &test_sources);
    tmod.addCSourceFiles(.{
        .root = b.path("tests/src"),
        .files = &.{"test_cache_bench.c"},
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-DMINIZ_NO_MALLOC", "-DMINIZ_NO_STDIO" },
    });
    const test_exe = b.addExecutable(.{ .name = "test_cache_bench", .root_module = tmod });
    const run_test = b.addRunArtifact(test_exe);
    const test_step = b.step("test", "Run the cache_bench contract tests");
    test_step.dependOn(&run_test.step);
}
