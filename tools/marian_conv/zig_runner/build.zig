//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host CLI runner executing `TMNT`/`TVOC`/`TSLT` translation containers
//! via `libs/ra8_translate` C23 ABI for converter golden parity verification.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const translate_lib = b.dependency("ra8_translate", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_translate");

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });
    exe_mod.linkLibrary(translate_lib);

    const executable = b.addExecutable(.{
        .name = "marian_zig_runner",
        .root_module = exe_mod,
    });
    b.installArtifact(executable);

    const run_tool = b.addRunArtifact(executable);
    run_tool.step.dependOn(b.getInstallStep());
    run_tool.addPassthruArgs();
    const run_step = b.step("run", "Run host Marian TMNT/TVOC/TSLT runner");
    run_step.dependOn(&run_tool.step);

    const test_options = b.addOptions();
    test_options.addOptionPath("tool_exe", executable.getEmittedBin());

    const runner_mod = b.createModule(.{
        .root_source_file = b.path("src/runner.zig"),
        .target = target,
        .optimize = optimize,
    });
    runner_mod.linkLibrary(translate_lib);

    const test_mod = b.createModule(.{
        .root_source_file = b.path("tests/runner_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_mod.addOptions("tool_options", test_options);
    test_mod.addImport("runner", runner_mod);
    test_mod.linkLibrary(translate_lib);

    const runner_tests = b.addTest(.{ .root_module = test_mod });
    const test_step = b.step("test", "Run marian_zig_runner unit and CLI tests");
    const run_tests = b.addRunArtifact(runner_tests);
    ra8_build.configureHostTestRun(b, test_step, runner_tests, run_tests);
    test_step.dependOn(&run_tests.step);
}
