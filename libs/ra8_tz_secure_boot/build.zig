//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_tz_secure_boot`.
//!
//! This branch builds and tests the decision core only: the register
//! geometry, the IPC attribution encoder, the partition validator, the NS
//! root-of-trust header reader and the PSAR gate planner. No static library
//! is installed and no symbol is exported yet, so CMake still compiles the C
//! translation units and the C ABI is untouched. The membrane
//! (`src/ra8_tz_secure_boot_abi.zig`) and the C deletion land next.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
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
    const run_internal_tests = b.addRunArtifact(internal_tests);

    // Each module's own tests. A test root that only *imports* the modules
    // does not pull their `test` blocks into the binary, so the decision core
    // is compiled a second time as its own test artifact. Without this the
    // suite runs the eight cross-module cases and silently skips the
    // thirty-two that hold each rule.
    const module_test_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const module_tests = b.addTest(.{ .root_module = module_test_module });
    const run_module_tests = b.addRunArtifact(module_tests);

    const test_step = b.step("test", "Run Zig ra8_tz_secure_boot tests");
    test_step.dependOn(&run_module_tests.step);
    test_step.dependOn(&run_internal_tests.step);
}
