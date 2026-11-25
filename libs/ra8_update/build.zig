//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_update`: the shared stage/verify/commit/rollback
//! pipeline seam and signed N-image bundle (`"RA8U"`) container core.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const err_mod = b.addModule("err", .{
        .root_source_file = b.path("src/internal/err.zig"),
        .target = target,
        .optimize = optimize,
    });

    const pipeline_mod = b.addModule("pipeline", .{
        .root_source_file = b.path("src/internal/pipeline.zig"),
        .target = target,
        .optimize = optimize,
    });
    pipeline_mod.addImport("err", err_mod);

    const session_mod = b.addModule("session", .{
        .root_source_file = b.path("src/internal/session.zig"),
        .target = target,
        .optimize = optimize,
    });
    session_mod.addImport("err", err_mod);
    session_mod.addImport("pipeline", pipeline_mod);

    const bundle_mod = b.addModule("bundle", .{
        .root_source_file = b.path("src/internal/bundle.zig"),
        .target = target,
        .optimize = optimize,
    });
    bundle_mod.addImport("err", err_mod);
    bundle_mod.addImport("pipeline", pipeline_mod);
    bundle_mod.addImport("session", session_mod);

    const reconcile_mod = b.addModule("reconcile", .{
        .root_source_file = b.path("src/internal/reconcile.zig"),
        .target = target,
        .optimize = optimize,
    });
    reconcile_mod.addImport("err", err_mod);
    reconcile_mod.addImport("bundle", bundle_mod);

    const abi_mod = b.createModule(.{
        .root_source_file = b.path("src/update_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_mod.addImport("err", err_mod);
    abi_mod.addImport("pipeline", pipeline_mod);
    abi_mod.addImport("session", session_mod);

    const bundle_abi_mod = b.createModule(.{
        .root_source_file = b.path("src/bundle_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    bundle_abi_mod.addImport("err", err_mod);
    bundle_abi_mod.addImport("pipeline", pipeline_mod);
    bundle_abi_mod.addImport("session", session_mod);
    bundle_abi_mod.addImport("bundle", bundle_mod);
    bundle_abi_mod.addImport("update_abi", abi_mod);

    const reconcile_abi_mod = b.createModule(.{
        .root_source_file = b.path("src/reconcile_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    reconcile_abi_mod.addImport("err", err_mod);
    reconcile_abi_mod.addImport("bundle", bundle_mod);
    reconcile_abi_mod.addImport("reconcile", reconcile_mod);

    const root_mod = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root_mod.addImport("update_abi", abi_mod);
    root_mod.addImport("bundle_abi", bundle_abi_mod);
    root_mod.addImport("reconcile_abi", reconcile_abi_mod);

    const library = b.addLibrary(.{
        .name = "ra8_update",
        .linkage = .static,
        .root_module = root_mod,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    library.installHeadersDirectory(b.path("inc"), "", .{});
    b.installArtifact(library);

    const pipeline_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/pipeline_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    pipeline_test_mod.addImport("err", err_mod);
    pipeline_test_mod.addImport("pipeline", pipeline_mod);
    const pipeline_tests = b.addTest(.{ .root_module = pipeline_test_mod });

    const bundle_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/bundle_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    bundle_test_mod.addImport("err", err_mod);
    bundle_test_mod.addImport("pipeline", pipeline_mod);
    bundle_test_mod.addImport("bundle", bundle_mod);
    const bundle_tests = b.addTest(.{ .root_module = bundle_test_mod });

    const reconcile_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/reconcile_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    reconcile_test_mod.addImport("err", err_mod);
    reconcile_test_mod.addImport("bundle", bundle_mod);
    reconcile_test_mod.addImport("reconcile", reconcile_mod);
    const reconcile_tests = b.addTest(.{ .root_module = reconcile_test_mod });

    const test_step = b.step("test", "Run Zig and C ra8_update tests");
    test_step.dependOn(&b.addRunArtifact(pipeline_tests).step);
    test_step.dependOn(&b.addRunArtifact(bundle_tests).step);
    test_step.dependOn(&b.addRunArtifact(reconcile_tests).step);

    if (target.result.os.tag != .freestanding) {
        const c_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        c_mod.addIncludePath(b.path("inc"));
        c_mod.addIncludePath(b.path("../ra8_core/inc"));
        c_mod.addCSourceFile(.{
            .file = b.path("tests/src/test_update_abi.c"),
            .flags = &.{ "-std=c23", "-Wall", "-Wextra", "-Werror" },
        });
        const c_exe = b.addExecutable(.{
            .name = "test_ra8_update_abi",
            .root_module = c_mod,
        });
        c_exe.root_module.linkLibrary(library);
        const run_c = b.addRunArtifact(c_exe);
        run_c.expectExitCode(0);
        test_step.dependOn(&run_c.step);
    }
}
