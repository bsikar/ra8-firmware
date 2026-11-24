//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the `roadmap_stats` host tool (RA8FW-335). One executable,
//! plus the test step `check_zig.py --test` (bsikar/ra8ci) runs: the parser
//! and renderer quirks, and the exit-status contract
//! `scripts/builders/roadmap_stats.sh` passes through.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    // Default target comes from the shared host probe so a native arm64 macOS
    // build links Zig's bundled libSystem stub instead of the SDK's (RA8FW-330).
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    const optimize = b.standardOptimizeOption(.{});

    const executable = b.addExecutable(.{
        .name = "roadmap_stats",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    b.installArtifact(executable);

    const run_tool = b.addRunArtifact(executable);
    run_tool.step.dependOn(b.getInstallStep());
    run_tool.addPassthruArgs();
    const run_step = b.step("run", "Refresh or check the ROADMAP.md summary block");
    run_step.dependOn(&run_tool.step);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const cli_module = b.createModule(.{
        .root_source_file = b.path("src/cli.zig"),
        .target = target,
        .optimize = optimize,
    });

    const internal_test_module = b.createModule(.{
        .root_source_file = b.path("tests/internal_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    internal_test_module.addImport("implementation", implementation_module);
    const internal_tests = b.addTest(.{ .root_module = internal_test_module });

    const cli_test_module = b.createModule(.{
        .root_source_file = b.path("tests/cli_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    cli_test_module.addImport("cli", cli_module);
    const cli_tests = b.addTest(.{ .root_module = cli_test_module });

    const test_step = b.step("test", "Run roadmap_stats tests");
    const run_internal_tests = b.addRunArtifact(internal_tests);
    ra8_build.configureHostTestRun(b, test_step, internal_tests, run_internal_tests);
    test_step.dependOn(&run_internal_tests.step);
    const run_cli_tests = b.addRunArtifact(cli_tests);
    ra8_build.configureHostTestRun(b, test_step, cli_tests, run_cli_tests);
    test_step.dependOn(&run_cli_tests.step);
}
