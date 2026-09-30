//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_tz_secure_boot`.
//!
//! Two roots: the decision core under `src/internal/`, and the C ABI membrane
//! over it. No static library is installed yet, so CMake still compiles the C
//! translation units and the #908 archive switch does not fire; the deletion
//! lands as its own visible change.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const root_of_trust = b.option(
        bool,
        "enable-root-of-trust",
        "Require the NS image to authenticate before BLXNS (RA8_ENABLE_ROOT_OF_TRUST)",
    ) orelse false;

    const options = b.addOptions();
    options.addOption(bool, "root_of_trust", root_of_trust);

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
    // is compiled a second time as its own test artifact.
    const module_test_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const module_tests = b.addTest(.{ .root_module = module_test_module });
    const run_module_tests = b.addRunArtifact(module_tests);

    // The membrane, compiled as its own test root so the exported functions
    // are exercised through the same signatures C will call them by.
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_tz_secure_boot_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_module.addOptions("build_options", options);

    const abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_test_module.addImport("abi", abi_module);

    const abi_tests = b.addTest(.{ .root_module = abi_test_module });
    const run_abi_tests = b.addRunArtifact(abi_tests);

    const test_step = b.step("test", "Run Zig ra8_tz_secure_boot tests");
    test_step.dependOn(&run_module_tests.step);
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
