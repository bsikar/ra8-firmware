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
        .link_libc = true,
    });
    // The real walk (RA8FW-1213) runs all-Zig since the CSS library port
    // (RA8FW-1217/1218/1219). RA8FW-1220: the layout core is Zig too, and
    // its extern seam (the four stbtt_* entry points) is linked into the
    // host test so the exported layout symbols resolve. RA8FW-1226: the
    // image layout TU is Zig as well.
    test_module.addIncludePath(b.path("inc"));
    test_module.addIncludePath(b.path("../../../libs/ra8_core/inc"));
    test_module.addIncludePath(b.path("../../../libs/ra8_mem/inc"));
    test_module.addIncludePath(b.path("../../../libs/ra8_imgdec/inc"));
    test_module.addIncludePath(b.path("../../../libs/ra8_gfx/inc"));
    test_module.addIncludePath(b.path("../../../libs/ra8_log/inc"));
    test_module.addIncludePath(b.path("../../../libs/ra8_webp/inc"));
    test_module.addIncludePath(b.path("../third_party/stb"));
    test_module.addCSourceFile(.{ .file = b.path("../third_party/stb/stb_truetype_impl.c"), .flags = &.{"-std=gnu2x"} });
    const reflow_parse_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(reflow_parse_test).step);
}
