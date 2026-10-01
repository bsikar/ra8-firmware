//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig half of `ra8_c6link`. CMake consumes the installed
//! static library and keeps compiling the library's remaining C, which calls
//! the ported `priv_c6link_*` symbols through the unchanged
//! `src/ra8_c6link_internal.h` declarations.
//!
//! No build options: the wire layers are pure byte work with no target
//! conditionals in them.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_c6link_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "ra8_c6link",
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
        .root_source_file = b.path("src/ra8_c6link_abi.zig"),
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

    const frame_test_module = b.createModule(.{
        .root_source_file = b.path("tests/frame_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    frame_test_module.addImport("implementation", implementation_module);
    const frame_tests = b.addTest(.{ .root_module = frame_test_module });

    const caps_test_module = b.createModule(.{
        .root_source_file = b.path("tests/caps_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    caps_test_module.addImport("implementation", implementation_module);
    const caps_tests = b.addTest(.{ .root_module = caps_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_frame_tests = b.addRunArtifact(frame_tests);
    const run_caps_tests = b.addRunArtifact(caps_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_c6link tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_frame_tests.step);
    test_step.dependOn(&run_caps_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
