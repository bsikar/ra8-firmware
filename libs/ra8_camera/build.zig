//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `ra8_camera` facade and its
//! memory source, JPEG passthrough codec, and software-JPEG codec. CMake
//! consumes the installed static library through the unchanged
//! `inc/ra8_camera*.h` C ABI.
//!
//! The archive root is `src/root.zig`: the facade and each backend live in
//! separate files, so each needs an explicit reference to reach the archive.
//!
//! No build options. The only link-time seam is `ra8_jpeg_sw_encode`, supplied
//! by `libs/ra8_jpeg` on every target, exactly as it was for the C backend.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "ra8_camera",
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
    const facade_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_camera_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    // One module root re-exporting the facade and every backend, so the ABI
    // membrane is compiled once per test binary.
    const backends_module = b.createModule(.{
        .root_source_file = b.path("src/backends.zig"),
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
    abi_test_module.addImport("facade", facade_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const backends_test_module = b.createModule(.{
        .root_source_file = b.path("tests/backends_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    backends_test_module.addImport("camera", backends_module);
    const backends_tests = b.addTest(.{ .root_module = backends_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_backends_tests = b.addRunArtifact(backends_tests);
    const test_step = b.step("test", "Run Zig ra8_camera tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_backends_tests.step);
}
