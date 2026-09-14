//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host-only C ABI fixture build graph. CMake consumes the installed static
//! library; the Zig test step verifies the private implementation and adapter.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const library = b.addLibrary(.{
        .name = "ra8_abi_fixture",
        .linkage = .static,
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/ra8_abi_fixture_abi.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    b.installArtifact(library);

    const tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/ra8_abi_fixture_abi.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run Zig ABI fixture tests");
    test_step.dependOn(&run_tests.step);
}
