//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_audio`. CMake consumes the
//! installed static library through the unchanged `inc/ra8_audio.h`,
//! `inc/ra8_audio_source_memory.h` and `inc/ra8_audio_source_pdm.h` C ABI.
//!
//! The archive root is `src/root.zig`: the facade and both backends live in
//! separate files, so each needs an explicit reference to reach the archive.
//!
//! No build options. Both backends are seam-injected on every target: the
//! memory source has no dependencies at all, and the PDM source binds the
//! `ra8_pdm_*` HAL and the millisecond clock as link-time externs.

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
        .name = "ra8_audio",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    // One module per test root, each rooted at a file that re-exports the
    // facade, so the ABI membrane is compiled once per test binary and the
    // types on both sides of a call are the same types.
    const audio_module = b.createModule(.{
        .root_source_file = b.path("src/source_memory.zig"),
        .target = target,
        .optimize = optimize,
    });
    const pdm_module = b.createModule(.{
        .root_source_file = b.path("src/source_pdm.zig"),
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
    abi_test_module.addImport("audio", audio_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const pdm_test_module = b.createModule(.{
        .root_source_file = b.path("tests/pdm_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    pdm_test_module.addImport("pdm", pdm_module);
    const pdm_tests = b.addTest(.{ .root_module = pdm_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_pdm_tests = b.addRunArtifact(pdm_tests);
    const test_step = b.step("test", "Run Zig ra8_audio tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_pdm_tests.step);
}
