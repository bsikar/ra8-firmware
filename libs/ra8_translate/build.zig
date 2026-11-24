//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_translate`: freestanding per-chapter `TRN1` sidecar
//! codec and future offline translation runtime primitives.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const err_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/err.zig"),
        .target = target,
        .optimize = optimize,
    });

    const crc32_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/crc32.zig"),
        .target = target,
        .optimize = optimize,
    });

    const sidecar_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/sidecar.zig"),
        .target = target,
        .optimize = optimize,
    });
    sidecar_mod.addImport("crc32", crc32_mod);
    sidecar_mod.addImport("err", err_mod);

    const lang_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/lang.zig"),
        .target = target,
        .optimize = optimize,
    });
    lang_mod.addImport("err", err_mod);
    lang_mod.addImport("sidecar", sidecar_mod);

    const abi_mod = b.createModule(.{
        .root_source_file = b.path("src/ra8_translate_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    abi_mod.addImport("err", err_mod);
    abi_mod.addImport("sidecar", sidecar_mod);
    abi_mod.addImport("lang", lang_mod);

    const library = b.addLibrary(.{
        .name = "ra8_translate",
        .linkage = .static,
        .root_module = abi_mod,
    });
    library.bundle_compiler_rt = false;
    library.link_function_sections = true;
    library.link_data_sections = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    // Keep the cross-target C23 header contract smoke compile on default_step.
    const smoke_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    smoke_mod.addIncludePath(b.path("inc"));
    smoke_mod.addIncludePath(b.path("../ra8_core/inc"));
    smoke_mod.addCSourceFile(.{
        .file = b.path("tests/ra8_translate_smoke.c"),
        .flags = &.{"-std=c23"},
    });
    const smoke = b.addObject(.{
        .name = "ra8_translate_smoke",
        .root_module = smoke_mod,
    });
    b.default_step.dependOn(&smoke.step);

    b.default_step.dependOn(&b.addInstallDirectory(.{
        .source_dir = b.path("inc"),
        .install_dir = .header,
        .install_subdir = "",
    }).step);

    const test_step = b.step("test", "Run Zig ra8_translate unit and ABI tests");

    const crc_tmod = b.createModule(.{
        .root_source_file = b.path("tests/crc32_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    crc_tmod.addImport("crc32", crc32_mod);
    const crc_tests = b.addTest(.{ .root_module = crc_tmod });
    const run_crc = b.addRunArtifact(crc_tests);
    ra8_build.configureHostTestRun(b, test_step, crc_tests, run_crc);
    test_step.dependOn(&run_crc.step);

    const sidecar_tmod = b.createModule(.{
        .root_source_file = b.path("tests/sidecar_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    sidecar_tmod.addImport("sidecar", sidecar_mod);
    const sidecar_tests = b.addTest(.{ .root_module = sidecar_tmod });
    const run_sidecar = b.addRunArtifact(sidecar_tests);
    ra8_build.configureHostTestRun(b, test_step, sidecar_tests, run_sidecar);
    test_step.dependOn(&run_sidecar.step);

    const abi_tmod = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_tmod.addImport("err", err_mod);
    abi_tmod.addImport("ra8_translate_abi", abi_mod);
    const abi_tests = b.addTest(.{ .root_module = abi_tmod });
    const run_abi = b.addRunArtifact(abi_tests);
    ra8_build.configureHostTestRun(b, test_step, abi_tests, run_abi);
    test_step.dependOn(&run_abi.step);

    const lang_tmod = b.createModule(.{
        .root_source_file = b.path("tests/lang_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    lang_tmod.addImport("lang", lang_mod);
    lang_tmod.addImport("sidecar", sidecar_mod);
    const lang_tests = b.addTest(.{ .root_module = lang_tmod });
    const run_lang = b.addRunArtifact(lang_tests);
    ra8_build.configureHostTestRun(b, test_step, lang_tests, run_lang);
    test_step.dependOn(&run_lang.step);
}
