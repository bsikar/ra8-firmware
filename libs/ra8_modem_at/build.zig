//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_modem_at`. CMake consumes
//! the installed static library through the unchanged `inc/ra8_modem_at.h` C
//! ABI; the `test` step covers the line accumulator, the classifier table and
//! the ABI membrane.
//!
//! No build options: the byte transport and the millisecond timebase are
//! caller-supplied seams on both the host and the target, so nothing about
//! this library is configured at compile time.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const test_helpers = b.option(bool, "test-helpers", "Export private helpers for host C tests") orelse false;

    const build_options = b.addOptions();
    build_options.addOption(bool, "test_helpers", test_helpers);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_modem_at_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    library_module.addOptions("build_options", build_options);

    const library = b.addLibrary(.{
        .name = if (test_helpers) "ra8_modem_at_test_helpers" else "ra8_modem_at",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_modem_at_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_module.addOptions("build_options", build_options);

    const internal_test_module = b.createModule(.{
        .root_source_file = b.path("tests/internal_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    internal_test_module.addImport("implementation", implementation_module);
    const internal_tests = b.addTest(.{ .root_module = internal_test_module });

    const abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_test_module.addImport("abi", abi_module);
    const test_helpers_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_modem_at_test_helpers.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_helpers_module.addImport("abi", abi_module);
    abi_test_module.addImport("test_helpers", test_helpers_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_modem_at tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
