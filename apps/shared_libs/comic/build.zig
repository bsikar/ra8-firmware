//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `comic`, exporting the static C library and public headers
//! in `inc/` with its upstream decompression and image-cache dependencies.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

const c_sources = [_][]const u8{
    "comic.c",
    "comic_cbr.c",
    "comic_cbt.c",
    "comic_cbz.c",
    "comic_tiles.c",
    "comic_wrapped.c",
    "ra8_rar.c",
    "ra8_rar5.c",
    "ra8_rar5_tables.c",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const core_artifact = if (target.result.os.tag == .freestanding) "ra8_core" else "ra8_core_zig";
    const core_lib = b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact(core_artifact);
    const mem_lib = b.dependency("ra8_mem", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_mem");
    const imgdec_lib = b.dependency("ra8_imgdec", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_imgdec");
    const unarch_dep = b.dependency("unarch", .{
        .target = target,
        .optimize = optimize,
    });
    const epub_miniz_lib = b.dependency("epub", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_epub_miniz");
    const jof_lib = b.dependency("jof", .{
        .target = target,
        .optimize = optimize,
    }).artifact("jof");
    const compress_dep = b.dependency("compress", .{
        .target = target,
        .optimize = optimize,
    });

    const library_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .pic = true,
        .sanitize_c = .off,
    });
    if (target.result.os.tag != .freestanding) library_module.link_libc = true;
    var flags: std.ArrayList([]const u8) = .empty;
    flags.appendSlice(b.allocator, &.{
        "-std=gnu23",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DMINIZ_NO_MALLOC",
        "-DMINIZ_NO_STDIO",
        "-DMINIZ_NO_TIME",
    }) catch @panic("OOM");
    if (target.result.os.tag != .freestanding) {
        flags.append(b.allocator, "-DRA8_OFF_TARGET") catch @panic("OOM");
        library_module.addIncludePath(compress_dep.artifact("miniz_archive").getEmittedIncludeTree());
    }
    library_module.addIncludePath(b.path("inc"));
    library_module.addIncludePath(b.path("src"));
    library_module.addIncludePath(core_lib.getEmittedIncludeTree());
    library_module.addIncludePath(mem_lib.getEmittedIncludeTree());
    library_module.addIncludePath(imgdec_lib.getEmittedIncludeTree());
    library_module.addIncludePath(unarch_dep.artifact("unarch_io").getEmittedIncludeTree());
    library_module.addIncludePath(unarch_dep.artifact("unarch_tar").getEmittedIncludeTree());
    library_module.addIncludePath(unarch_dep.artifact("unarch_gzip").getEmittedIncludeTree());
    library_module.addIncludePath(unarch_dep.artifact("unarch_xz").getEmittedIncludeTree());
    library_module.addIncludePath(epub_miniz_lib.getEmittedIncludeTree());
    library_module.addIncludePath(jof_lib.getEmittedIncludeTree());
    library_module.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &c_sources,
        .flags = flags.items,
    });

    const library = b.addLibrary(.{
        .name = "comic",
        .linkage = .static,
        .root_module = library_module,
    });
    library.installHeadersDirectory(b.path("inc"), "", .{});
    if (target.result.os.tag != .freestanding) {
        b.installArtifact(library);
    }

    const smoke_files = b.addWriteFiles();
    const smoke_c = smoke_files.add("comic_header_smoke.c",
        \\#include "comic.h"
        \\#include "comic_tiles.h"
        \\
        \\int ra8_comic_header_smoke_symbol(void) {
        \\    return (int)sizeof(comic_page_t);
        \\}
        \\
    );
    const smoke_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    if (target.result.os.tag != .freestanding) smoke_mod.link_libc = true;
    smoke_mod.addIncludePath(library.getEmittedIncludeTree());
    smoke_mod.addIncludePath(core_lib.getEmittedIncludeTree());
    smoke_mod.addIncludePath(mem_lib.getEmittedIncludeTree());
    smoke_mod.addIncludePath(unarch_dep.artifact("unarch_io").getEmittedIncludeTree());
    smoke_mod.addIncludePath(unarch_dep.artifact("unarch_tar").getEmittedIncludeTree());
    smoke_mod.addIncludePath(unarch_dep.artifact("unarch_gzip").getEmittedIncludeTree());
    smoke_mod.addIncludePath(unarch_dep.artifact("unarch_xz").getEmittedIncludeTree());
    smoke_mod.addIncludePath(epub_miniz_lib.getEmittedIncludeTree());
    smoke_mod.addIncludePath(jof_lib.getEmittedIncludeTree());
    smoke_mod.addCSourceFile(.{
        .file = smoke_c,
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror" },
    });
    const smoke_obj = b.addObject(.{
        .name = "comic_header_smoke",
        .root_module = smoke_mod,
    });

    const test_step = b.step("test", "Build comic static library and verify public headers");
    test_step.dependOn(&library.step);
    test_step.dependOn(&smoke_obj.step);
}
