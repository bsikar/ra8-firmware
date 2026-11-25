//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `jof`. CMake consumes the
//! installed static library through the unchanged headers in `inc/`.
//!
//! The template is libs/ra8_imgdec/build.zig (RA8FW-1139): module + static
//! library + host test step. The test units fill in as functions port.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");
const Translator = @import("translate_c").Translator;

/// The headers jof_abi.zig exposes, translated once so every Zig unit shares
/// their exact layouts. The build writes the include list, so no C is added.
const jof_header =
    \\#include "jof_internal.h"
    \\#include "jof_png_internal.h"
    \\#include "jof_audit.h"
    \\#include "ra8_webp.h"
    \\
;

fn translateHeaders(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
    miniz_include_dir: std.Build.LazyPath,
    webp_include_dir: std.Build.LazyPath,
    core_include_dir: std.Build.LazyPath,
    imgdec_include_dir: std.Build.LazyPath,
    mem_include_dir: std.Build.LazyPath,
) *std.Build.Module {
    const header = b.addWriteFiles().add("jof_c.h", jof_header);
    const translator: Translator = .init(b.dependency("translate_c", .{}), .{
        .c_source_file = header,
        .target = target,
        .optimize = optimize,
        .link_libc = false,
    });
    translator.defineCMacro("static_assert", "_Static_assert");
    translator.defineCMacro("alignas", "_Alignas");
    // miniz.h pulls <time.h>, which the translate-c resource dir lacks; the
    // macro makes miniz substitute its own dummy time type and skips the
    // include. Layout-neutral: tinfl types are untouched.
    // Mirror the C build's miniz carve-down set (tools/rabook_imagepack
    // miniz_flags) so the seam sees the same miniz.h branches as the C
    // TUs. MINIZ_NO_TIME also skips <time.h>, which aro lacks.
    translator.defineCMacro("MINIZ_NO_ARCHIVE_APIS", "1");
    translator.defineCMacro("MINIZ_NO_MALLOC", "1");
    translator.defineCMacro("MINIZ_NO_STDIO", "1");
    translator.defineCMacro("MINIZ_NO_TIME", "1");
    // aro drops every declaration that follows a mid-file #pragma once,
    // and miniz.h (a concatenation of miniz's sources) carries five, so
    // the seam shim replaces it with the type surface the seam needs:
    // exact-size mz_* typedefs and an opaque tinfl_decompressor, which
    // jof_png_internal.h holds by pointer only. The shim also supplies
    // the libc headers aro ships none of (assert/stdlib/string/stdio).
    // The C builds never see this path.
    translator.addIncludePath(b.path("seam_shim"));
    translator.addIncludePath(b.path("src"));
    translator.addIncludePath(miniz_include_dir);
    translator.addIncludePath(b.path("inc"));
    translator.addIncludePath(webp_include_dir);
    translator.addIncludePath(core_include_dir);
    translator.addIncludePath(imgdec_include_dir);
    translator.addIncludePath(mem_include_dir);
    return translator.mod;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const miniz_include_dir = b.dependency("compress", .{
        .target = target,
        .optimize = optimize,
    }).namedWriteFiles("miniz_include").getDirectory();
    const webp_lib = b.dependency("webp", .{
        .target = target,
        .optimize = optimize,
    }).artifact("webp");
    const core_lib = b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_core");
    const imgdec_lib = b.dependency("ra8_imgdec", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_imgdec");
    const mem_lib = b.dependency("ra8_mem", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_mem");

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/jof_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const jof_h_mod = translateHeaders(
        b,
        target,
        optimize,
        miniz_include_dir,
        webp_lib.getEmittedIncludeTree(),
        core_lib.getEmittedIncludeTree(),
        imgdec_lib.getEmittedIncludeTree(),
        mem_lib.getEmittedIncludeTree(),
    );
    library_module.addImport("jof_h", jof_h_mod);
    const library = b.addLibrary(.{
        .name = "jof",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    library.installHeadersDirectory(b.path("inc"), "", .{});
    b.installArtifact(library);

    // Export C test suite sources + internal headers needed by host tools
    // running the portable JOF unit binaries without exposing raw src/ paths.
    const c_tests = b.addNamedWriteFiles("c_tests");
    const jof_test_files = [_][]const u8{
        "test_jof_audit.c",
        "test_jof.c",
        "test_jof_edges.c",
        "test_jof_png_hostile.c",
        "test_jof_probe_dims.c",
        "test_jof_produce.c",
        "test_jof_produce_guards.c",
        "test_jof_produce_reject.c",
        "test_jof_produce_webp.c",
    };
    for (jof_test_files) |file_name| {
        _ = c_tests.addCopyFile(b.path(b.fmt("tests/src/{s}", .{file_name})), file_name);
    }
    _ = c_tests.addCopyFile(b.path("tests/inc/jof_png_hostile_fixture.h"), "jof_png_hostile_fixture.h");
    _ = c_tests.addCopyFile(b.path("src/jof_internal.h"), "jof_internal.h");
    _ = c_tests.addCopyFile(b.path("src/jof_png_internal.h"), "jof_png_internal.h");

    const test_step = b.step("test", "Run Zig jof tests");
    // The streaming trio calls sibling C symbols whose real definitions live
    // in the consumers' C TUs; the standalone test link stubs them
    // (tests/abi_stubs.zig). The stubs ride a dedicated test module so they
    // never enter libjof.a or any consumer link.
    const sut_module = b.createModule(.{
        .root_source_file = b.path("src/jof_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    sut_module.addImport("jof_h", jof_h_mod);
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/jof_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addImport("jof_h", jof_h_mod);
    test_module.addImport("jof_abi", sut_module);
    const webp_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(webp_test).step);
}
