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
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const update_dep = b.dependency("ra8_update", .{
        .target = target,
        .optimize = optimize,
    });
    const update_err = update_dep.module("err");
    const update_pipeline = update_dep.module("pipeline");
    const update_session = update_dep.module("session");
    const update_bundle = update_dep.module("bundle");

    // The orchestration membrane imports the parsing one and the shared
    // pipeline adapter (`ota_pipeline.zig` via relative import), so this single
    // root pulls every exported symbol of the library into one archive without
    // duplicate ra8_update_* exports.
    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_ota_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    library_module.addImport("err", update_err);
    library_module.addImport("pipeline", update_pipeline);
    library_module.addImport("session", update_session);
    library_module.addImport("bundle", update_bundle);

    const library = b.addLibrary(.{
        .name = "ra8_ota",
        .linkage = .static,
        .root_module = library_module,
    });
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;

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
    library.root_module.addObject(reset_hook_object);
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
    orchestration_module.addImport("err", update_err);
    orchestration_module.addImport("pipeline", update_pipeline);
    orchestration_module.addImport("session", update_session);
    orchestration_module.addImport("bundle", update_bundle);

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
    orchestration_test_module.addImport("pipeline", update_pipeline);
    orchestration_test_module.addImport("bundle", update_bundle);
    const orchestration_tests = b.addTest(.{ .root_module = orchestration_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_orchestration_tests = b.addRunArtifact(orchestration_tests);
    const test_step = b.step("test", "Run Zig ra8_ota tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_orchestration_tests.step);

    if (target.result.os.tag != .freestanding) {
        const c_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        c_mod.addIncludePath(b.path("inc"));
        c_mod.addIncludePath(b.path("../ra8_core/inc"));
        c_mod.addIncludePath(b.path("../ra8_update/inc"));
        c_mod.addCSourceFile(.{
            .file = b.path("tests/src/test_ota_bundle_abi.c"),
            .flags = &.{ "-std=c23", "-Wall", "-Wextra", "-Werror" },
        });
        const c_exe = b.addExecutable(.{
            .name = "test_ra8_ota_bundle_abi",
            .root_module = c_mod,
        });
        c_exe.root_module.linkLibrary(library);
        const run_c = b.addRunArtifact(c_exe);
        run_c.expectExitCode(0);
        test_step.dependOn(&run_c.step);
    }
}
