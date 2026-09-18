//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_ota`. CMake consumes the
//! installed static library through the unchanged `inc/ra8_ota.h` and
//! `src/ra8_ota_internal.h`; the `test` step covers the pure scanners, the
//! parsing membrane and the orchestration/verify state machine.
//!
//! No build options: nothing in this cluster is configured at compile time.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // The orchestration membrane imports the parsing one, so this single
    // root pulls every exported symbol of the library into one archive.
    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_ota_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "ra8_ota",
        .linkage = .static,
        .root_module = library_module,
    });

    // The weak `ra8_ota_system_reset_hook` default must be its own archive
    // member, or the call in `ra8_ota_commit_and_reboot` resolves to it inside
    // the same object and a strong override in the image stops being reached.
    const reset_hook_object = b.addObject(.{
        .name = "ra8_ota_reset_hook",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/reset_hook.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    library.addObject(reset_hook_object);
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_ota_parse_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const orchestration_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_ota_abi.zig"),
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

    const orchestration_test_module = b.createModule(.{
        .root_source_file = b.path("tests/orchestration_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    orchestration_test_module.addImport("abi", orchestration_module);
    const orchestration_tests = b.addTest(.{ .root_module = orchestration_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_orchestration_tests = b.addRunArtifact(orchestration_tests);
    const test_step = b.step("test", "Run Zig ra8_ota tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_orchestration_tests.step);
}
