//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the epub library:
//! src/epub_abi.zig (the decompression guard, RA8FW-1160),
//! src/epub_fs_abi.zig (the ra8_fs bridge, RA8FW-1161),
//! src/epub_open_abi.zig (the open/close core, RA8FW-1162) and
//! src/epub_chapter_abi.zig (the chapter/glyph accessors, RA8FW-1163), all behind the
//! unchanged C headers in inc/. CMake consumes the installed static
//! libraries through those headers. Template: apps/shared_libs/xml/build.zig
//! (plain static libraries + host test step). The epub core is Zig and
//! enters through the archive link; the test step links the teststub C
//! stand-ins for miniz, the decompression policy, the ra8_fs seam and the
//! epub core entry points.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
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
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

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
    fs_library.bundle_compiler_rt = fs_library.root_module.resolved_target.?.result.os.tag != .freestanding;
    fs_library.root_module.pic = true;
    b.installArtifact(fs_library);

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
    open_library.bundle_compiler_rt = open_library.root_module.resolved_target.?.result.os.tag != .freestanding;
    open_library.root_module.pic = true;
    b.installArtifact(open_library);

    const test_step = b.step("test", "Run Zig epub guard, fs-bridge and open tests");
    const test_module = b.createModule(.{
        .root_source_file = b.path("src/epub_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addCSourceFile(.{ .file = b.path("teststub/zip_guard_test_stubs.c"), .flags = &.{"-std=c23"} });
    const epub_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(epub_test).step);

    const fs_test_module = b.createModule(.{
        .root_source_file = b.path("src/epub_fs_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    fs_test_module.addCSourceFile(.{ .file = b.path("teststub/fs_bridge_test_stubs.c"), .flags = &.{"-std=c23"} });
    const fs_test = b.addTest(.{ .root_module = fs_test_module });
    test_step.dependOn(&b.addRunArtifact(fs_test).step);
    const open_test_module = b.createModule(.{
        .root_source_file = b.path("src/epub_open_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
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
    chapter_library.bundle_compiler_rt = chapter_library.root_module.resolved_target.?.result.os.tag != .freestanding;
    chapter_library.root_module.pic = true;
    b.installArtifact(chapter_library);

    const chapter_test_module = b.createModule(.{
        .root_source_file = b.path("src/epub_chapter_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
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
    entry_library.bundle_compiler_rt = entry_library.root_module.resolved_target.?.result.os.tag != .freestanding;
    entry_library.root_module.pic = true;
    b.installArtifact(entry_library);

    const entry_test_module = b.createModule(.{
        .root_source_file = b.path("src/epub_entry_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    entry_test_module.addCSourceFile(.{ .file = b.path("teststub/entry_test_stubs.c"), .flags = &.{"-std=c23"} });
    const entry_test = b.addTest(.{ .root_module = entry_test_module });
    test_step.dependOn(&b.addRunArtifact(entry_test).step);

    // The strict container/OPF XML consumers ship as their own archive
    // (RA8FW-1164) for the same pull granularity: a consumer that never
    // parses container or OPF XML never pulls the shim. Its externs are the
    // ra8_xml reader and decoder, which the epub XML shim and the remaining
    // C consumer epub_xml_toc.c both reach through the archive link.
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
    xml_shim_library.bundle_compiler_rt = xml_shim_library.root_module.resolved_target.?.result.os.tag != .freestanding;
    xml_shim_library.root_module.pic = true;
    b.installArtifact(xml_shim_library);

    // The shim test links the real ra8_xml reader and decoder, so the tests
    // replay true XML documents end to end rather than standing in for the
    // parser.
    const xml_shim_test_module = b.createModule(.{
        .root_source_file = b.path("src/epub_xml_shim_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const xml_module = b.createModule(.{
        .root_source_file = b.path("../xml/src/xml_abi.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    const xml_library = b.addLibrary(.{
        .name = "ra8_xml_shim_test",
        .linkage = .static,
        .root_module = xml_module,
    });
    xml_module.addCSourceFile(.{ .file = b.path("../xml/src/xml_doctype.c"), .flags = &.{"-std=c23"} });
    xml_module.addIncludePath(b.path("../xml/inc"));
    xml_module.addIncludePath(b.path("../xml/src"));
    xml_module.addIncludePath(b.path("../../../libs/ra8_core/inc"));
    xml_shim_test_module.link_libc = true;
    const xml_shim_test = b.addTest(.{ .root_module = xml_shim_test_module });
    xml_shim_test_module.linkLibrary(xml_library);
    test_step.dependOn(&b.addRunArtifact(xml_shim_test).step);
}
