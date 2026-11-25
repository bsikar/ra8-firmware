//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig reflow parse + tokenize port (RA8FW-1212 +
//! RA8FW-1213): src/reflow_parse_abi.zig is the public parse entry point
//! `reflow_parse_xhtml` and src/reflow_tokenize.zig is the markup walk
//! `priv_reflow_xml_walk`, both behind the unchanged C headers in inc/.
//! The entities unit and the CSS library stay C and are linked in for the
//! host test step (the lexical + attribute primitives are Zig since RA8FW-1214/1215); CMake consumes the installed static
//! library through inc/reflow_api.h.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/reflow_parse_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });

    // RA8FW-1263: when the LiteHTML v2 engine is linked (CMake passes
    // -Dreflow-v2-engine=true under REFLOW_USE_LITEHTML), it owns the core
    // reflow ABI and the archive keeps only the auxiliary seams (loader /
    // font binding, link + anchor queries). The default (v1) build keeps
    // every export.
    const reflow_v2_engine = b.option(
        bool,
        "reflow-v2-engine",
        "The LiteHTML v2 engine owns the core reflow ABI",
    ) orelse false;
    const build_options = b.addOptions();
    build_options.addOption(bool, "reflow_v2_engine", reflow_v2_engine);
    library_module.addOptions("reflow_build_options", build_options);
    const core_lib = b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_core");
    const mem_lib = b.dependency("ra8_mem", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_mem");
    const imgdec_dep = b.dependency("ra8_imgdec", .{
        .target = target,
        .optimize = optimize,
    });
    const imgdec_lib = imgdec_dep.artifact("ra8_imgdec");
    const gfx_lib = b.dependency("ra8_gfx", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_gfx");
    const webp_lib = b.dependency("webp", .{
        .target = target,
        .optimize = optimize,
    }).artifact("webp");

    // Owned vendored stb implementations (`stb_truetype` for host/libc tests,
    // `stb_truetype_arena` with `ra8_stbtt_malloc` hooks, and `stb_image` over
    // `ra8_imgdec`'s arena hooks).
    const stb_tt_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .pic = true,
        .sanitize_c = .off,
    });
    if (target.result.os.tag != .freestanding) stb_tt_mod.link_libc = true;
    stb_tt_mod.addIncludePath(b.path("../third_party/stb"));
    stb_tt_mod.addCSourceFile(.{
        .file = b.path("../third_party/stb/stb_truetype_impl.c"),
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fno-strict-aliasing" },
    });
    const stb_tt_lib = b.addLibrary(.{
        .name = "stb_truetype",
        .linkage = .static,
        .root_module = stb_tt_mod,
    });
    stb_tt_lib.installHeader(b.path("../third_party/stb/stb_truetype.h"), "stb_truetype.h");

    const stb_tt_arena_src = b.addWriteFiles().add("stb_truetype_arena.c",
        \\#include "ra8_stbtt_alloc.h"
        \\#define STBTT_malloc(x, u) ra8_stbtt_malloc(x)
        \\#define STBTT_free(x, u) ra8_stbtt_free(x)
        \\#include "stb_truetype_impl.c"
        \\
    );
    const stb_tt_arena_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .pic = true,
        .sanitize_c = .off,
    });
    if (target.result.os.tag != .freestanding) stb_tt_arena_mod.link_libc = true;
    stb_tt_arena_mod.addIncludePath(b.path("../third_party/stb"));
    stb_tt_arena_mod.addIncludePath(b.path("inc"));
    stb_tt_arena_mod.addIncludePath(core_lib.getEmittedIncludeTree());
    stb_tt_arena_mod.addIncludePath(mem_lib.getEmittedIncludeTree());
    stb_tt_arena_mod.addIncludePath(imgdec_lib.getEmittedIncludeTree());
    stb_tt_arena_mod.addCSourceFile(.{
        .file = stb_tt_arena_src,
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fno-strict-aliasing" },
    });
    const stb_tt_arena_lib = b.addLibrary(.{
        .name = "stb_truetype_arena",
        .linkage = .static,
        .root_module = stb_tt_arena_mod,
    });
    stb_tt_arena_lib.installHeader(b.path("../third_party/stb/stb_truetype.h"), "stb_truetype.h");

    const stb_img_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .pic = true,
        .sanitize_c = .off,
    });
    if (target.result.os.tag != .freestanding) stb_img_mod.link_libc = true;
    stb_img_mod.addIncludePath(b.path("../third_party/stb"));
    stb_img_mod.addIncludePath(b.path("inc"));
    stb_img_mod.addIncludePath(core_lib.getEmittedIncludeTree());
    stb_img_mod.addIncludePath(mem_lib.getEmittedIncludeTree());
    stb_img_mod.addIncludePath(imgdec_lib.getEmittedIncludeTree());
    stb_img_mod.addCSourceFile(.{
        .file = b.path("../third_party/stb/stb_image_impl.c"),
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fno-strict-aliasing" },
    });
    const stb_img_lib = b.addLibrary(.{
        .name = "stb_image",
        .linkage = .static,
        .root_module = stb_img_mod,
    });
    stb_img_lib.installHeader(b.path("../third_party/stb/stb_image.h"), "stb_image.h");
    const stb_include = b.addNamedWriteFiles("stb_include");
    _ = stb_include.addCopyFile(b.path("../third_party/stb/stb_image.h"), "stb_image.h");
    _ = stb_include.addCopyFile(b.path("../third_party/stb/stb_truetype.h"), "stb_truetype.h");
    if (target.result.os.tag != .freestanding) {
        b.installArtifact(stb_tt_lib);
        b.installArtifact(stb_tt_arena_lib);
        b.installArtifact(stb_img_lib);
    }

    const library = b.addLibrary(.{
        .name = "ra8_reflow_parse",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    library.link_function_sections = true;
    library.link_data_sections = true;
    library.installHeadersDirectory(b.path("inc"), "", .{});
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig reflow parse entry tests");
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/reflow_parse_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    test_module.addImport("reflow", library_module);
    test_module.addIncludePath(library.getEmittedIncludeTree());
    test_module.addIncludePath(core_lib.getEmittedIncludeTree());
    test_module.addIncludePath(mem_lib.getEmittedIncludeTree());
    test_module.addIncludePath(imgdec_lib.getEmittedIncludeTree());
    test_module.addIncludePath(gfx_lib.getEmittedIncludeTree());
    test_module.addIncludePath(webp_lib.getEmittedIncludeTree());
    test_module.linkLibrary(stb_tt_lib);
    test_module.addImport("ra8_stbtt_guard", imgdec_dep.module("stbtt_guard"));
    const reflow_parse_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(reflow_parse_test).step);
    test_step.dependOn(&stb_tt_arena_lib.step);
    test_step.dependOn(&stb_img_lib.step);
}
