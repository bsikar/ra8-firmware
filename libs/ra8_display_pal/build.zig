//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the backend-agnostic half of
//! `ra8_display_pal`: the dispatcher in `inc/ra8_display_pal.h` and the
//! page-turn refresh policy in `inc/ra8_display_pal_policy.h`. CMake consumes
//! the installed static library through those unchanged C headers; the `test`
//! step covers the pure decision logic and the ABI membrane.
//!
//! No build options: the panel is a caller-supplied vtable on both the host
//! and the target, so nothing here is configured at compile time.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_display_pal_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "ra8_display_pal",
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
        .root_source_file = b.path("src/ra8_display_pal_abi.zig"),
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
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_display_pal tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
