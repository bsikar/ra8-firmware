//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_cache_store`. CMake consumes
//! the installed static archive through the unchanged `inc/ra8_cache_store.h`
//! C ABI; the `test` step covers the pure logic, the runtime ABI membrane and
//! the mount / recovery path.
//!
//! The archive root is `src/root.zig` because the library is two files that
//! meet at the `priv_cache_store_*` symbols, and both have to be part of the
//! compilation for their exports to land. The tests deliberately do not share
//! that root: `tests/abi_test.zig` substitutes its own helpers over a RAM
//! medium, which only works while `mount.zig` is out of that binary.
//!
//! No build options: the store has no compile-time switches.

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
        .name = "ra8_cache_store",
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
        .root_source_file = b.path("src/ra8_cache_store_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const mount_module = b.createModule(.{
        .root_source_file = b.path("src/mount.zig"),
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

    const mount_test_module = b.createModule(.{
        .root_source_file = b.path("tests/mount_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    mount_test_module.addImport("mount", mount_module);
    const mount_tests = b.addTest(.{ .root_module = mount_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_mount_tests = b.addRunArtifact(mount_tests);
    const test_step = b.step("test", "Run Zig ra8_cache_store tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_mount_tests.step);
}
