//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the `merge_ihex` host tool (#858). One executable, plus the
//! test step `scripts/checks/check_zig.py --test` runs: the record algebra and
//! the exit-status contract the firmware builds depend on.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const executable = b.addExecutable(.{
        .name = "merge_ihex",
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
    const run_step = b.step("run", "Merge two Intel HEX files");
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

    const test_step = b.step("test", "Run merge_ihex tests");
    test_step.dependOn(&b.addRunArtifact(internal_tests).step);
    test_step.dependOn(&b.addRunArtifact(cli_tests).step);

    const docs_object = b.addObject(.{
        .name = "merge_ihex_docs",
        .root_module = executable.root_module,
    });
    const install_docs = b.addInstallDirectory(.{
        .source_dir = docs_object.getEmittedDocs(),
        .install_dir = .prefix,
        .install_subdir = "merge_ihex",
    });
    const docs_step = b.step("docs", "Emit Zig autodoc HTML into <prefix>/merge_ihex");
    docs_step.dependOn(&install_docs.step);
}
