//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host-only C ABI fixture build graph. CMake consumes the installed static
//! library; the Zig test step verifies the private implementation and adapter.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    // Default target comes from the shared host probe so a native arm64 macOS
    // build links Zig's bundled libSystem stub instead of the SDK's (#899).
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    const optimize = b.standardOptimizeOption(.{});
    const library = b.addLibrary(.{
        .name = "ra8_abi_fixture",
        .linkage = .static,
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/ra8_abi_fixture_abi.zig"),
            .target = target,
            .optimize = optimize,
            // The Rust and C consumers link position-independent executables,
            // so every object in this archive, bundled compiler_rt included,
            // has to be position-independent too.
            .pic = true,
        }),
    });
    // CMake links the installed archive with the system linker, which cannot see
    // Zig's compiler_rt. Bundle it into the archive so stack-probe helpers such
    // as __zig_probe_stack resolve without the consumer knowing about Zig.
    library.bundle_compiler_rt = true;
    b.installArtifact(library);

    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_abi_fixture_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_test_module.addImport("abi", abi_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });
    const internal_test_module = b.createModule(.{
        .root_source_file = b.path("tests/internal_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    internal_test_module.addImport("implementation", implementation_module);
    const internal_tests = b.addTest(.{ .root_module = internal_test_module });
    const test_step = b.step("test", "Run Zig ABI fixture tests");
    _ = ra8_build.addHostTestRun(b, test_step, abi_tests);
    _ = ra8_build.addHostTestRun(b, test_step, internal_tests);
}
