//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_usb_pal`. CMake consumes the
//! installed static library through the unchanged `inc/ra8_usb_pal.h` C ABI;
//! the `test` step covers the per-endpoint ring and the ABI membrane.
//!
//! No build options: the Ring-3 `ra8_usb` driver and the logger are link-time
//! seams on both the host and the target, so nothing here is configured at
//! compile time.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_usb_pal_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "ra8_usb_pal",
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
        .root_source_file = b.path("src/ra8_usb_pal_abi.zig"),
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

    const descriptor_module = b.createModule(.{
        .root_source_file = b.path("src/internal/descriptor.zig"),
        .target = target,
        .optimize = optimize,
    });
    const descriptor_test_module = b.createModule(.{
        .root_source_file = b.path("tests/descriptor_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    descriptor_test_module.addImport("descriptor", descriptor_module);
    const descriptor_tests = b.addTest(.{ .root_module = descriptor_test_module });

    const desc_abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_usb_desc_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const desc_abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/desc_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    desc_abi_test_module.addImport("desc_abi", desc_abi_module);
    const desc_abi_tests = b.addTest(.{ .root_module = desc_abi_test_module });

    const compose_module = b.createModule(.{
        .root_source_file = b.path("src/internal/compose.zig"),
        .target = target,
        .optimize = optimize,
    });
    const compose_test_module = b.createModule(.{
        .root_source_file = b.path("tests/compose_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    compose_test_module.addImport("compose", compose_module);
    const compose_tests = b.addTest(.{ .root_module = compose_test_module });

    const compose_abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_usb_compose_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const compose_abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/compose_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    compose_abi_test_module.addImport("compose_abi", compose_abi_module);
    const compose_abi_tests = b.addTest(.{ .root_module = compose_abi_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_usb_pal tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&b.addRunArtifact(descriptor_tests).step);
    test_step.dependOn(&b.addRunArtifact(desc_abi_tests).step);
    test_step.dependOn(&b.addRunArtifact(compose_tests).step);
    test_step.dependOn(&b.addRunArtifact(compose_abi_tests).step);
}
