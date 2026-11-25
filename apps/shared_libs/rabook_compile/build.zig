//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `rabook_compile`, exporting the core RABOOK1 builder /
//! container emitter library and its host test fixture helper.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

const c_sources = [_][]const u8{
    "rabook_compile.c",
    "ra8_rabook_finalize.c",
    "ra8_rabook_container.c",
    "ra8_rabook_comic.c",
    "ra8_rabook_gray4.c",
    "ra8_rabook_xml_shim.c",
    "ra8_rabook_raster.c",
    "ra8_rabook_pipeline.c",
    "ra8_rabook_pipeline_fs.c",
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
    const fs_lib = b.dependency("ra8_fs", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_fs");
    const imgdec_lib = b.dependency("ra8_imgdec", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_imgdec");
    const jpeg_lib = b.dependency("ra8_jpeg", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_jpeg");
    const book_lib = b.dependency("book", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_book");
    const compress_dep = b.dependency("compress", .{
        .target = target,
        .optimize = optimize,
    });
    const compress_lib = compress_dep.artifact("compress");
    const webp_lib = b.dependency("webp", .{
        .target = target,
        .optimize = optimize,
    }).artifact("webp");
    const xml_lib = b.dependency("xml", .{
        .target = target,
        .optimize = optimize,
    }).artifact("xml_reader");
    const epub_dep = b.dependency("epub", .{
        .target = target,
        .optimize = optimize,
    });
    const epub_lib = epub_dep.artifact("ra8_epub");
    const epub_miniz_lib = epub_dep.artifact("ra8_epub_miniz");
    const reflow_dep = b.dependency("reflow", .{
        .target = target,
        .optimize = optimize,
    });
    const reflow_lib = reflow_dep.artifact("ra8_reflow_parse");

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
        "-DMINIZ_NO_ARCHIVE_APIS",
        "-DMINIZ_NO_MALLOC",
        "-DMINIZ_NO_STDIO",
        "-DMINIZ_NO_TIME",
    }) catch @panic("OOM");
    if (target.result.os.tag != .freestanding) {
        flags.append(b.allocator, "-DRA8_OFF_TARGET") catch @panic("OOM");
        library_module.addIncludePath(compress_dep.artifact("miniz").getEmittedIncludeTree());
        library_module.addIncludePath(reflow_dep.artifact("stb_image").getEmittedIncludeTree());
    } else {
        library_module.addIncludePath(compress_dep.namedWriteFiles("miniz_include").getDirectory());
        library_module.addIncludePath(reflow_dep.namedWriteFiles("stb_include").getDirectory());
    }
    library_module.addIncludePath(b.path("inc"));
    library_module.addIncludePath(b.path("src"));
    library_module.addIncludePath(core_lib.getEmittedIncludeTree());
    library_module.addIncludePath(mem_lib.getEmittedIncludeTree());
    library_module.addIncludePath(fs_lib.getEmittedIncludeTree());
    library_module.addIncludePath(imgdec_lib.getEmittedIncludeTree());
    library_module.addIncludePath(jpeg_lib.getEmittedIncludeTree());
    library_module.addIncludePath(book_lib.getEmittedIncludeTree());
    library_module.addIncludePath(compress_lib.getEmittedIncludeTree());
    library_module.addIncludePath(webp_lib.getEmittedIncludeTree());
    library_module.addIncludePath(xml_lib.getEmittedIncludeTree());
    library_module.addIncludePath(epub_lib.getEmittedIncludeTree());
    library_module.addIncludePath(epub_miniz_lib.getEmittedIncludeTree());
    library_module.addIncludePath(reflow_lib.getEmittedIncludeTree());
    library_module.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &c_sources,
        .flags = flags.items,
    });

    const library = b.addLibrary(.{
        .name = "rabook_compile",
        .linkage = .static,
        .root_module = library_module,
    });
    library.installHeadersDirectory(b.path("inc"), "", .{});

    const fixture_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    if (target.result.os.tag != .freestanding) fixture_module.link_libc = true;
    fixture_module.addIncludePath(b.path("tests/inc"));
    fixture_module.addIncludePath(b.path("inc"));
    fixture_module.addIncludePath(core_lib.getEmittedIncludeTree());
    fixture_module.addIncludePath(book_lib.getEmittedIncludeTree());
    fixture_module.addIncludePath(b.path("../../../tests/support/inc"));
    fixture_module.addCSourceFile(.{
        .file = b.path("tests/src/rabook_compile_test_fixture.c"),
        .flags = flags.items,
    });
    const fixture_lib = b.addLibrary(.{
        .name = "rabook_compile_fixture",
        .linkage = .static,
        .root_module = fixture_module,
    });
    fixture_lib.installHeadersDirectory(b.path("tests/inc"), "", .{});

    if (target.result.os.tag != .freestanding) {
        b.installArtifact(library);
        b.installArtifact(fixture_lib);
    }

    const smoke_files = b.addWriteFiles();
    const smoke_c = smoke_files.add("rabook_compile_header_smoke.c",
        \\#include "ra8_rabook_container.h"
        \\#include "rabook_compile.h"
        \\
        \\int ra8_rabook_compile_header_smoke_symbol(void) {
        \\    return (int)sizeof(ra8_rabook_ctx_t);
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
    smoke_mod.addIncludePath(book_lib.getEmittedIncludeTree());
    smoke_mod.addCSourceFile(.{
        .file = smoke_c,
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror" },
    });
    const smoke_obj = b.addObject(.{
        .name = "rabook_compile_header_smoke",
        .root_module = smoke_mod,
    });

    const test_step = b.step("test", "Build rabook_compile library and verify public headers");
    test_step.dependOn(&library.step);
    test_step.dependOn(&fixture_lib.step);
    test_step.dependOn(&smoke_obj.step);
}
