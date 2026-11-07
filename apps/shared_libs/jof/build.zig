//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `jof`. CMake consumes the
//! installed static library through the unchanged headers in `inc/`.
//!
//! The template is libs/ra8_imgdec/build.zig (RA8FW-1139): module + static
//! library + host test step. The test units fill in as functions port.

const std = @import("std");
const Translator = @import("translate_c").Translator;

/// The headers jof_abi.zig exposes, translated once so every Zig unit shares
/// their exact layouts. The build writes the include list, so no C is added.
const jof_header =
    \\#include "jof_internal.h"
    \\#include "ra8_webp.h"
    \\
;

fn translateHeaders(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
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
    translator.addIncludePath(b.path("src"));
    translator.addIncludePath(b.path("inc"));
    translator.addIncludePath(b.path("../webp/inc"));
    translator.addIncludePath(b.path("../../../libs/ra8_core/inc"));
    translator.addIncludePath(b.path("../../../libs/ra8_imgdec/inc"));
    translator.addIncludePath(b.path("../../../libs/ra8_mem/inc"));
    return translator.mod;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/jof_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const jof_h_mod = translateHeaders(b, target, optimize);
    library_module.addImport("jof_h", jof_h_mod);
    const library = b.addLibrary(.{
        .name = "jof",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig jof tests");
    // The streaming trio calls sibling C symbols whose real definitions live
    // in the consumers' C TUs; the standalone test link stubs them
    // (tests/abi_stubs.zig). The stubs ride a dedicated test module so they
    // never enter libjof.a or any consumer link.
    const test_module = b.createModule(.{
        .root_source_file = b.path("src/jof_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    test_module.addImport("jof_h", jof_h_mod);
    const stub_mod = b.createModule(.{
        .root_source_file = b.path("tests/abi_stubs.zig"),
        .target = target,
        .optimize = optimize,
    });
    stub_mod.addImport("jof_h", jof_h_mod);
    const stubs = b.addObject(.{ .name = "abi_stubs", .root_module = stub_mod });
    test_module.addObject(stubs);
    const webp_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(webp_test).step);
}
