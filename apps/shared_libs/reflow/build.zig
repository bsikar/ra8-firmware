//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig reflow parse entry (RA8FW-1212):
//! src/reflow_parse_abi.zig, the public parse entry point
//! `reflow_parse_xhtml` behind the unchanged C headers in inc/. The
//! tokenizer it dispatches into stays C (src/reflow_tokenize.c) and
//! provides the extern `priv_reflow_xml_walk`; CMake consumes the
//! installed static library through inc/reflow_api.h. Template:
//! apps/shared_libs/xml/build.zig (plain static library + host test
//! step); the test build stands in for the C tokenizer with the stub
//! exported from src/reflow_parse_tests.zig.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/reflow_parse_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_reflow_parse",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig reflow parse entry tests");
    const test_module = b.createModule(.{
        .root_source_file = b.path("src/reflow_parse_tests.zig"),
        .target = target,
        .optimize = optimize,
    });
    const reflow_parse_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(reflow_parse_test).step);
}
