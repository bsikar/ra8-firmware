//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the curated font assets and baked header declarations of
//! `ra8_fonts`.
//!
//! Exposes:
//! - Zig module `"ra8_fonts"`: target-independent font catalog and SFNT helpers.
//! - Static artifact `"ra8_fonts"`: header-bearing library installing
//!   `literata_latin1.h` on dependent include paths via `linkLibrary`.
//! - Named WriteFiles `"include"`: `literata_latin1.h` directory for tools and
//!   translators that consume the header path directly.
//! - Named WriteFiles `"assets"`: packaged `.ttf` assets under their documented
//!   relative paths (`literata_latin1.ttf`, `Literata-Regular.ttf`,
//!   `RA8UISans/RA8UISans-*.ttf`).

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    _ = b.addModule("ra8_fonts", .{
        .root_source_file = b.path("src/fonts.zig"),
    });

    const include_files = b.addNamedWriteFiles("include");
    _ = include_files.addCopyFile(b.path("literata_latin1.h"), "literata_latin1.h");

    const asset_files = b.addNamedWriteFiles("assets");
    _ = asset_files.addCopyFile(b.path("Literata-Regular.ttf"), "Literata-Regular.ttf");
    _ = asset_files.addCopyFile(b.path("literata_latin1.ttf"), "literata_latin1.ttf");
    _ = asset_files.addCopyFile(b.path("RA8UISans/RA8UISans-Regular.ttf"), "RA8UISans/RA8UISans-Regular.ttf");
    _ = asset_files.addCopyFile(b.path("RA8UISans/RA8UISans-Bold.ttf"), "RA8UISans/RA8UISans-Bold.ttf");

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/fonts.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_fonts",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.installHeader(b.path("literata_latin1.h"), "literata_latin1.h");
    b.installArtifact(library);

    const catalog_module = b.createModule(.{
        .root_source_file = b.path("src/fonts.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/fonts_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addImport("ra8_fonts", catalog_module);
    const unit_tests = b.addTest(.{ .root_module = test_module });

    // Verify that linking `ra8_fonts` puts `literata_latin1.h` on a C consumer's
    // include path without exposing package source paths manually.
    const smoke_files = b.addWriteFiles();
    const smoke_c = smoke_files.add("header_smoke.c",
        \\#include "literata_latin1.h"
        \\
        \\const unsigned char *ra8_fonts_header_probe_data(void) {
        \\    return g_ra8_font_literata_latin1;
        \\}
        \\
    );
    const smoke_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    smoke_mod.addCSourceFile(.{
        .file = smoke_c,
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror" },
    });
    smoke_mod.addIncludePath(library.getEmittedIncludeTree());
    const smoke_obj = b.addObject(.{
        .name = "ra8_fonts_header_smoke",
        .root_module = smoke_mod,
    });
    unit_tests.root_module.linkLibrary(library);

    const test_step = b.step("test", "Run Zig ra8_fonts tests");
    test_step.dependOn(&b.addRunArtifact(unit_tests).step);
    test_step.dependOn(&smoke_obj.step);
}
