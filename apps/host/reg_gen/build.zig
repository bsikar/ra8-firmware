//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Explicit reg_gen build and test graph. CI discovers this root and invokes
//! `zig build test`; CMake remains the host-tool integration boundary.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const executable = b.addExecutable(.{
        .name = "reg_gen",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    b.installArtifact(executable);

    const application_module = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addImport("application", application_module);
    const tests = b.addTest(.{ .root_module = test_module });
    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run reg_gen unit and C23 contract tests");
    test_step.dependOn(&run_tests.step);

    const docs_object = b.addObject(.{
        .name = "reg_gen_docs",
        .root_module = executable.root_module,
    });
    const install_docs = b.addInstallDirectory(.{
        .source_dir = docs_object.getEmittedDocs(),
        .install_dir = .prefix,
        .install_subdir = "reg_gen",
    });
    const docs_step = b.step("docs", "Emit Zig autodoc HTML into <prefix>/reg_gen");
    docs_step.dependOn(&install_docs.step);
}
