//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the `gen_jpeg_fixture` host tool (#858). One executable,
//! plus the test step `scripts/checks/check_zig.py --test` runs: the blob the
//! libFuzzer corpora are seeded with and the exit-status contract
//! `scripts/builders/init_fuzz_corpora.sh` depends on.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const executable = b.addExecutable(.{
        .name = "gen_jpeg_fixture",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    b.installArtifact(executable);

    const run_tool = b.addRunArtifact(executable);
    run_tool.step.dependOn(b.getInstallStep());
    if (b.args) |args| run_tool.addArgs(args);
    const run_step = b.step("run", "Write one minimal baseline JPEG seed");
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

    const test_step = b.step("test", "Run gen_jpeg_fixture tests");
    test_step.dependOn(&b.addRunArtifact(internal_tests).step);
    test_step.dependOn(&b.addRunArtifact(cli_tests).step);
}
