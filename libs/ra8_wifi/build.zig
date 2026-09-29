//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `ra8_wifi` facade. CMake
//! consumes the installed static library through the unchanged
//! `inc/ra8_wifi.h` C ABI; the `test` step covers the lifecycle core and the
//! ABI membrane.
//!
//! No build options: the radio is a caller-supplied vtable on both the host
//! and the target, so nothing about this library is configured at compile
//! time.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_wifi_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "ra8_wifi",
        .linkage = .static,
        .root_module = library_module,
    });
    // Host tests link this archive with the system toolchain, and Rust
    // consumers link it into PIE executables.
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_wifi_abi.zig"),
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

    const abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_test_module.addImport("abi", abi_module);
    abi_test_module.addIncludePath(b.path("../ra8_core/inc"));
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });
    abi_tests.addCSourceFile(.{
        .file = b.path("tests/log_fixture.c"),
        .flags = &.{ "-std=c23", "-Wall", "-Wextra", "-Werror" },
    });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_wifi tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
