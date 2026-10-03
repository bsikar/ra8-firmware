//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_app`. CMake consumes the
//! installed static library through the unchanged `inc/ra8_app.h`,
//! `inc/ra8_appimg.h` and `inc/ra8_appimg_verify.h` C ABIs; the `test` step
//! covers the registry core, the `.ra8app` format, the admission gate and the
//! ABI membrane.
//!
//! There is no build option here, deliberately: the C translation unit this
//! replaces took no `-D` of its own, so adding one would widen the library's
//! contract rather than port it.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_app_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });

    const library = b.addLibrary(.{
        .name = "ra8_app",
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
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_app_abi.zig"),
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

    const appimg_module = b.createModule(.{
        .root_source_file = b.path("src/internal/appimg.zig"),
        .target = target,
        .optimize = optimize,
    });
    const appimg_verify_module = b.createModule(.{
        .root_source_file = b.path("src/internal/appimg_verify.zig"),
        .target = target,
        .optimize = optimize,
    });

    const appimg_test_module = b.createModule(.{
        .root_source_file = b.path("tests/appimg_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    appimg_test_module.addImport("appimg", appimg_module);
    const appimg_tests = b.addTest(.{ .root_module = appimg_test_module });

    const appimg_verify_test_module = b.createModule(.{
        .root_source_file = b.path("tests/appimg_verify_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    appimg_verify_test_module.addImport("appimg_verify", appimg_verify_module);
    const appimg_verify_tests = b.addTest(.{ .root_module = appimg_verify_test_module });

    const appimg_pack_module = b.createModule(.{
        .root_source_file = b.path("src/internal/appimg_pack.zig"),
        .target = target,
        .optimize = optimize,
    });
    const appimg_pack_test_module = b.createModule(.{
        .root_source_file = b.path("tests/appimg_pack_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    appimg_pack_test_module.addImport("appimg_pack", appimg_pack_module);
    const appimg_pack_tests = b.addTest(.{ .root_module = appimg_pack_test_module });

    // A ThreadX module binary into a signed `.ra8app` (RA8FW-474). The tool
    // runs on the build machine whatever this library is built for.
    const module_pack_module = b.createModule(.{
        .root_source_file = b.path("src/internal/module_pack.zig"),
        .target = target,
        .optimize = optimize,
    });
    const pack_tool = b.addExecutable(.{
        .name = "ra8app_pack",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tools/ra8app_pack.zig"),
            .target = b.graph.host,
            .optimize = optimize,
        }),
    });
    pack_tool.root_module.addImport("module_pack", b.createModule(.{
        .root_source_file = b.path("src/internal/module_pack.zig"),
        .target = b.graph.host,
        .optimize = optimize,
    }));
    b.installArtifact(pack_tool);

    var module_pack_tests: [2]*std.Build.Step.Compile = undefined;
    inline for (.{ "tests/txm_preamble_test.zig", "tests/module_pack_test.zig" }, 0..) |path, index| {
        const test_module = b.createModule(.{
            .root_source_file = b.path(path),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport("module_pack", module_pack_module);
        module_pack_tests[index] = b.addTest(.{ .root_module = test_module });
    }

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_appimg_tests = b.addRunArtifact(appimg_tests);
    const run_appimg_verify_tests = b.addRunArtifact(appimg_verify_tests);
    const run_appimg_pack_tests = b.addRunArtifact(appimg_pack_tests);
    const test_step = b.step("test", "Run Zig ra8_app tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_appimg_tests.step);
    test_step.dependOn(&run_appimg_verify_tests.step);
    test_step.dependOn(&run_appimg_pack_tests.step);
    for (module_pack_tests) |compile| test_step.dependOn(&b.addRunArtifact(compile).step);
    // The tool must at least build on every test run.
    test_step.dependOn(&pack_tool.step);
}
