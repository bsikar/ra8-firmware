//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `xml` reader: the decode
//! half (RA8FW-1157) and the markup scanner (RA8FW-1158) behind the
//! unchanged C headers in inc/. CMake consumes the installed static library
//! through those headers. Template: apps/shared_libs/compress/build.zig
//! (plain static library + host test step). The DOCTYPE validator is still
//! C (xml_doctype.c, slice 3): the scanner calls it through the archive
//! link, so the host test links that TU directly.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/xml_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "xml_reader",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig xml reader tests");
    const test_module = b.createModule(.{
        .root_source_file = b.path("src/xml_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addCSourceFile(.{ .file = b.path("src/xml_doctype.c"), .flags = &.{"-std=c23"} });
    test_module.addIncludePath(b.path("inc"));
    test_module.addIncludePath(b.path("src"));
    test_module.addIncludePath(b.path("../../../libs/ra8_core/inc"));
    test_module.link_libc = true;
    const xml_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(xml_test).step);
}
