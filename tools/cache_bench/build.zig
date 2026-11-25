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
const ra8_build = @import("ra8_zig_build");

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

const c_flags = [_][]const u8{
    "-std=gnu23",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-DMINIZ_NO_ARCHIVE_APIS",
    "-DMINIZ_NO_MALLOC",
    "-DMINIZ_NO_STDIO",
    "-DMINIZ_NO_TIME",
};

fn addSharedSources(b: *std.Build, mod: *std.Build.Module, files: []const []const u8) void {
    mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = files,
        .flags = &c_flags,
    });
}

fn configureTargetModule(
    b: *std.Build,
    mod: *std.Build.Module,
    core_include_dir: std.Build.LazyPath,
    mem_lib: *std.Build.Step.Compile,
    book_lib: *std.Build.Step.Compile,
    miniz_lib: *std.Build.Step.Compile,
    files: []const []const u8,
) void {
    mod.link_libc = true;
    mod.linkLibrary(mem_lib);
    mod.linkLibrary(book_lib);
    mod.linkLibrary(miniz_lib);
    mod.addIncludePath(b.path("inc"));
    mod.addIncludePath(core_include_dir);
    addSharedSources(b, mod, files);
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    // The benchmark measures, so the default build optimizes like the C-era
    // tool build did (-O2); an explicit -Doptimize still wins.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    const core_include_dir = b.dependency("ra8_core", .{ .target = target, .optimize = optimize }).artifact("ra8_core").getEmittedIncludeTree();
    const mem_dep = b.dependency("ra8_mem", .{ .target = target, .optimize = optimize });
    const mem_lib = mem_dep.artifact("ra8_mem");
    const book_lib = b.dependency("ra8_book", .{ .target = target, .optimize = optimize }).artifact("ra8_book");
    const miniz_lib = b.dependency("compress", .{ .target = target, .optimize = optimize }).artifact("miniz");

    const mod = b.createModule(.{ .target = target, .optimize = optimize });
    configureTargetModule(b, mod, core_include_dir, mem_lib, book_lib, miniz_lib, &bench_sources);
    const exe = b.addExecutable(.{ .name = "cache_bench", .root_module = mod });

    b.installArtifact(exe);

    const tmod = b.createModule(.{ .target = target, .optimize = optimize });
    configureTargetModule(b, tmod, core_include_dir, mem_lib, book_lib, miniz_lib, &test_sources);
    tmod.addCSourceFiles(.{
        .root = b.path("tests/src"),
        .files = &.{"test_cache_bench.c"},
        .flags = &c_flags,
    });
    const test_exe = b.addExecutable(.{ .name = "test_cache_bench", .root_module = tmod });
    const run_test = b.addRunArtifact(test_exe);
    const test_step = b.step("test", "Run the cache_bench contract tests");
    test_step.dependOn(&run_test.step);

    const test_options = b.addOptions();
    test_options.addOptionPath("tool_exe", exe.getEmittedBin());
    const ztest_mod = b.createModule(.{
        .root_source_file = b.path("tests/cache_bench_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    ztest_mod.addOptions("tool_options", test_options);
    const ztest = b.addTest(.{ .root_module = ztest_mod });
    const run_ztest = b.addRunArtifact(ztest);
    ra8_build.configureHostTestRun(b, test_step, ztest, run_ztest);
    test_step.dependOn(&run_ztest.step);
}
