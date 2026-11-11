//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `xml` decode half
//! (RA8FW-1157). CMake consumes the installed static library through the
//! unchanged headers in inc/. Template: apps/shared_libs/compress/build.zig
//! (plain static library + host test step; the decode half is pure, so no
//! seam and no translated header module are needed).

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

    const test_step = b.step("test", "Run Zig xml decode tests");
    const xml_test = b.addTest(.{ .root_module = library_module });
    test_step.dependOn(&b.addRunArtifact(xml_test).step);
}
