//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `longstrip` (RA8FW-1156). CMake
//! consumes the installed static library through the unchanged header in
//! `inc/`. The template is apps/shared_libs/jof/build.zig (the module +
//! static library + host test step shape, translated seam included).

const std = @import("std");
const ra8_build = @import("ra8_zig_build");
const Translator = @import("translate_c").Translator;

/// The header longstrip_abi.zig exposes, translated once so every Zig unit
/// shares their exact layouts. The build writes the include list, so no C is
/// added.
const longstrip_header =
    \\#include "longstrip.h"
    \\
;

fn translateHeaders(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
    jof_include_dir: std.Build.LazyPath,
    core_include_dir: std.Build.LazyPath,
    mem_include_dir: std.Build.LazyPath,
) *std.Build.Module {
    const header = b.addWriteFiles().add("longstrip_c.h", longstrip_header);
    const translator: Translator = .init(b.dependency("translate_c", .{}), .{
        .c_source_file = header,
        .target = target,
        .optimize = optimize,
        .link_libc = false,
    });
    translator.defineCMacro("static_assert", "_Static_assert");
    translator.defineCMacro("alignas", "_Alignas");
    // aro ships none of the libc headers, so the seam shim supplies the ones
    // the header set pulls (assert/stdio/stdlib/string) and stands in for any
    // vendor header with a mid-file #pragma once. The C builds never see this
    // path.
    translator.addIncludePath(b.path("seam_shim"));
    translator.addIncludePath(b.path("inc"));
    translator.addIncludePath(jof_include_dir);
    translator.addIncludePath(core_include_dir);
    translator.addIncludePath(mem_include_dir);
    return translator.mod;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const jof_lib = b.dependency("jof", .{
        .target = target,
        .optimize = optimize,
    }).artifact("jof");
    const core_lib = b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_core");
    const mem_lib = b.dependency("ra8_mem", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_mem");

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/longstrip_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const ls_h_mod = translateHeaders(
        b,
        target,
        optimize,
        jof_lib.getEmittedIncludeTree(),
        core_lib.getEmittedIncludeTree(),
        mem_lib.getEmittedIncludeTree(),
    );
    library_module.addImport("longstrip_h", ls_h_mod);
    const library = b.addLibrary(.{
        .name = "longstrip",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    library.installHeadersDirectory(b.path("inc"), "", .{});
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig longstrip tests");
    // The JOF reader, tile cache and log backend the engine drives are the
    // deterministic fakes in tests/abi_stubs.zig. The stubs ride a dedicated
    // test module so they never enter liblongstrip.a or any consumer link.
    const sut_module = b.createModule(.{
        .root_source_file = b.path("src/longstrip_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    sut_module.addImport("longstrip_h", ls_h_mod);
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/longstrip_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addImport("longstrip_h", ls_h_mod);
    test_module.addImport("longstrip_abi", sut_module);
    const ls_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(ls_test).step);
}
