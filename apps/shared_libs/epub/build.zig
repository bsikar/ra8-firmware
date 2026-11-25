//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the epub library:
//! src/epub_abi.zig (the decompression guard, RA8FW-1160),
//! src/epub_fs_abi.zig (the ra8_fs bridge, RA8FW-1161),
//! src/epub_open_abi.zig (the open/close core, RA8FW-1162) and
//! src/epub_chapter_abi.zig (the chapter/glyph accessors, RA8FW-1163),
//! src/epub_img_import_abi.zig (the import-time transcode wiring, RA8FW-1193) and
//! src/epub_img_tiles_abi.zig (the tile binder + reflow img loader, RA8FW-1194), all behind the
//! unchanged C headers in inc/. CMake consumes the installed static
//! libraries through those headers. Template: apps/shared_libs/xml/build.zig
//! (plain static libraries + host test step). The epub core is Zig and
//! enters through the archive link; the test step links the teststub C
//! stand-ins for miniz, the decompression policy, the ra8_fs seam and the
//! epub core entry points.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/epub_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_epub",
        .linkage = .static,
        .root_module = library_module,
    });
    installStaticLib(b, library, &.{"epub.h"});

    // The ra8_fs bridge ships as its own archive so a core-only consumer
    // (no ra8_fs linked) never pulls the bridge's ra8_fs externs: archive
    // members are pulled only when something references the bridge's three
    // exports, which is exactly the __has_include("ra8_fs.h") gate the C TU
    // expressed.
    const fs_module = b.createModule(.{
        .root_source_file = b.path("src/epub_fs_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const fs_library = b.addLibrary(.{
        .name = "ra8_epub_fs",
        .linkage = .static,
        .root_module = fs_module,
    });
    installStaticLib(b, fs_library, &.{"epub_fs.h"});

    // The epub open/close core ships as its own archive for the same pull
    // granularity as the fs bridge: its externs (miniz, the miniz arena, the
    // XML shim, the policy preflight and the guard exports) only load when a
    // consumer references the open/close entry points.
    const open_module = b.createModule(.{
        .root_source_file = b.path("src/epub_open_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const open_library = b.addLibrary(.{
        .name = "ra8_epub_open",
        .linkage = .static,
        .root_module = open_module,
    });
    installStaticLib(b, open_library, &.{});

    const test_step = b.step("test", "Run Zig epub guard, fs-bridge and open tests");
    const test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/epub_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/epub_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addImport("epub_abi", test_module_sut);
    test_module.addCSourceFile(.{ .file = b.path("teststub/zip_guard_test_stubs.c"), .flags = &.{"-std=c23"} });
    const epub_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(epub_test).step);

    const fs_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/epub_fs_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const fs_test_module = b.createModule(.{
        .root_source_file = b.path("tests/epub_fs_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    fs_test_module.addImport("epub_fs_abi", fs_test_module_sut);
    fs_test_module.addCSourceFile(.{ .file = b.path("teststub/fs_bridge_test_stubs.c"), .flags = &.{"-std=c23"} });
    const fs_test = b.addTest(.{ .root_module = fs_test_module });
    test_step.dependOn(&b.addRunArtifact(fs_test).step);
    const open_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/epub_open_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const open_test_module = b.createModule(.{
        .root_source_file = b.path("tests/epub_open_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    open_test_module.addImport("epub_open_abi", open_test_module_sut);
    open_test_module.addCSourceFile(.{ .file = b.path("teststub/open_test_stubs.c"), .flags = &.{"-std=c23"} });
    const open_test = b.addTest(.{ .root_module = open_test_module });
    test_step.dependOn(&b.addRunArtifact(open_test).step);

    // The chapter/glyph accessors ship as their own archive (RA8FW-1163) for
    // the same pull granularity: a consumer that never reads chapter/glyph
    // data never pulls the miniz or stbtt externs. The test step links
    // stub-driven miniz / zip-guard / stbtt / sfnt-guard stand-ins.
    const chapter_module = b.createModule(.{
        .root_source_file = b.path("src/epub_chapter_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const chapter_library = b.addLibrary(.{
        .name = "ra8_epub_chapter",
        .linkage = .static,
        .root_module = chapter_module,
    });
    installStaticLib(b, chapter_library, &.{});

    const chapter_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/epub_chapter_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const chapter_test_module = b.createModule(.{
        .root_source_file = b.path("tests/epub_chapter_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    chapter_test_module.addImport("epub_chapter_abi", chapter_test_module_sut);
    chapter_test_module.addCSourceFile(.{ .file = b.path("teststub/chapter_test_stubs.c"), .flags = &.{"-std=c23"} });
    const chapter_test = b.addTest(.{ .root_module = chapter_test_module });
    test_step.dependOn(&b.addRunArtifact(chapter_test).step);

    // The bounded-RAM entry accessors ship as their own archive (RA8FW-1165)
    // for the same pull granularity: a consumer that never streams or windows
    // an entry never pulls the miniz iterator externs. The test step links
    // stub-driven miniz locate/stat/iterator and sibling priv_* stand-ins.
    const entry_module = b.createModule(.{
        .root_source_file = b.path("src/epub_entry_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const entry_library = b.addLibrary(.{
        .name = "ra8_epub_entry",
        .linkage = .static,
        .root_module = entry_module,
    });
    installStaticLib(b, entry_library, &.{"epub_entry.h"});

    const entry_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/epub_entry_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const entry_test_module = b.createModule(.{
        .root_source_file = b.path("tests/epub_entry_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    entry_test_module.addImport("epub_entry_abi", entry_test_module_sut);
    entry_test_module.addCSourceFile(.{ .file = b.path("teststub/entry_test_stubs.c"), .flags = &.{"-std=c23"} });
    const entry_test = b.addTest(.{ .root_module = entry_test_module });
    test_step.dependOn(&b.addRunArtifact(entry_test).step);

    // The strict container/OPF XML consumers ship as their own archive
    // (RA8FW-1164) for the same pull granularity: a consumer that never
    // parses container or OPF XML never pulls the shim. Its externs are the
    // ra8_xml reader and decoder, which the epub XML shim and the TOC
    // TU both reach through the archive link.
    const xml_shim_module = b.createModule(.{
        .root_source_file = b.path("src/epub_xml_shim_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const xml_shim_library = b.addLibrary(.{
        .name = "ra8_epub_xml",
        .linkage = .static,
        .root_module = xml_shim_module,
    });
    installStaticLib(b, xml_shim_library, &.{});

    // The shim test links the real ra8_xml reader and decoder, so the tests
    // replay true XML documents end to end rather than standing in for the
    // parser.
    const xml_shim_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/epub_xml_shim_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const xml_shim_test_module = b.createModule(.{
        .root_source_file = b.path("tests/epub_xml_shim_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    xml_shim_test_module.addImport("epub_xml_shim_abi", xml_shim_test_module_sut);
    const xml_library = b.dependency("xml", .{
        .target = target,
        .optimize = optimize,
    }).artifact("xml_reader");
    xml_shim_test_module.link_libc = true;
    const xml_shim_test = b.addTest(.{ .root_module = xml_shim_test_module });
    xml_shim_test_module.linkLibrary(xml_library);
    test_step.dependOn(&b.addRunArtifact(xml_shim_test).step);

    // The NCX and nav TOC consumers (RA8FW-1166) ship as their own archive
    // for the same pull granularity: a consumer that never parses a
    // navigation document never pulls the TOC TU.
    const toc_module = b.createModule(.{
        .root_source_file = b.path("src/epub_xml_toc_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const toc_library = b.addLibrary(.{
        .name = "ra8_epub_toc",
        .linkage = .static,
        .root_module = toc_module,
    });
    installStaticLib(b, toc_library, &.{});

    // The TOC test links the real ra8_xml reader and the real shim helpers,
    // so the tests replay true NCX and nav documents end to end.
    const toc_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/epub_xml_toc_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const toc_test_module = b.createModule(.{
        .root_source_file = b.path("tests/epub_xml_toc_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    toc_test_module.addImport("epub_xml_toc_abi", toc_test_module_sut);
    const toc_shim_module = b.createModule(.{
        .root_source_file = b.path("src/epub_xml_shim_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const toc_shim_library = b.addLibrary(.{
        .name = "ra8_epub_xml_toc_test",
        .linkage = .static,
        .root_module = toc_shim_module,
    });
    toc_test_module.link_libc = true;
    toc_test_module.linkLibrary(toc_shim_library);
    toc_test_module.linkLibrary(xml_library);
    const toc_test = b.addTest(.{ .root_module = toc_test_module });
    test_step.dependOn(&b.addRunArtifact(toc_test).step);

    // The miniz arena allocator (RA8FW-1167) ships as its own archive for the
    // same pull granularity: its exports only load when a consumer wires the
    // miniz callbacks through the arena descriptor.
    const miniz_module = b.createModule(.{
        .root_source_file = b.path("src/epub_miniz_alloc_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const miniz_library = b.addLibrary(.{
        .name = "ra8_epub_miniz",
        .linkage = .static,
        .root_module = miniz_module,
    });
    installStaticLib(b, miniz_library, &.{"epub_miniz_alloc.h"});

    // The allocator test needs no stand-ins: the TU has no externs, so the
    // tests drive a real workspace end to end.
    const miniz_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/epub_miniz_alloc_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const miniz_test_module = b.createModule(.{
        .root_source_file = b.path("tests/epub_miniz_alloc_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    miniz_test_module.addImport("epub_miniz_alloc_abi", miniz_test_module_sut);
    const miniz_test = b.addTest(.{ .root_module = miniz_test_module });
    test_step.dependOn(&b.addRunArtifact(miniz_test).step);

    // The import-time transcode wiring ships as its own archive (RA8FW-1193)
    // for the same pull granularity: its externs (the entry cursor quartet,
    // the binder registration pair and jof_produce) only load when a consumer
    // calls epub_tile_binder_import. This is the C TU's __has_include gate,
    // moved to the build: an epub-only app that never links jof keeps the
    // same link surface the empty C translation unit gave it.
    const img_import_module = b.createModule(.{
        .root_source_file = b.path("src/epub_img_import_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const img_import_library = b.addLibrary(.{
        .name = "ra8_epub_img_import",
        .linkage = .static,
        .root_module = img_import_module,
    });
    installStaticLib(b, img_import_library, &.{});

    const img_import_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/epub_img_import_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const img_import_test_module = b.createModule(.{
        .root_source_file = b.path("tests/epub_img_import_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    img_import_test_module.addImport("epub_img_import_abi", img_import_test_module_sut);
    img_import_test_module.addCSourceFile(.{ .file = b.path("teststub/img_import_test_stubs.c"), .flags = &.{"-std=c23"} });
    const img_import_test = b.addTest(.{ .root_module = img_import_test_module });
    test_step.dependOn(&b.addRunArtifact(img_import_test).step);

    // The tile binder + reflow <img> loader ship as their own archive
    // (RA8FW-1194) for the same pull granularity: its externs (the entry
    // trio, epub_get_resource, the jof pair and the tile-cache quartet) only
    // load when a consumer calls the binder or the loader. This is the C
    // TU's __has_include gate, moved to the build: an epub-only app that
    // never links jof keeps the same link surface the empty C translation
    // unit gave it.
    const img_tiles_module = b.createModule(.{
        .root_source_file = b.path("src/epub_img_tiles_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const img_tiles_library = b.addLibrary(.{
        .name = "ra8_epub_img_tiles",
        .linkage = .static,
        .root_module = img_tiles_module,
    });
    installStaticLib(b, img_tiles_library, &.{"epub_img_tiles.h"});

    const img_tiles_test_module_sut = b.createModule(.{
        .root_source_file = b.path("src/epub_img_tiles_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const img_tiles_test_module = b.createModule(.{
        .root_source_file = b.path("tests/epub_img_tiles_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    img_tiles_test_module.addImport("epub_img_tiles_abi", img_tiles_test_module_sut);
    img_tiles_test_module.addCSourceFile(.{ .file = b.path("teststub/tiles_test_stubs.c"), .flags = &.{"-std=c23"} });
    const img_tiles_test = b.addTest(.{ .root_module = img_tiles_test_module });
    test_step.dependOn(&b.addRunArtifact(img_tiles_test).step);
}

fn installStaticLib(b: *std.Build, lib: *std.Build.Step.Compile, headers: []const []const u8) void {
    lib.bundle_compiler_rt = lib.root_module.resolved_target.?.result.os.tag != .freestanding;
    // Split functions and data so --gc-sections discards unused EPUB entry points on ARM.
    lib.link_function_sections = true;
    lib.link_data_sections = true;
    lib.root_module.pic = true;
    for (headers) |hdr| {
        lib.installHeader(b.path(b.fmt("inc/{s}", .{hdr})), hdr);
    }
    b.installArtifact(lib);
}
