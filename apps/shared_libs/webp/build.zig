//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig WebP facade (RA8FW-1209, slice 1): the decode
//! facade and its bump-arena hooks behind the unchanged C headers in inc/.
//! CMake keeps globbing the C facade for consumers until the package flip
//! slice; this package test drives the real vendored decoder against the Zig
//! hooks with -DRA8_WEBP_USE_ARENA, proving the decode stays heap-free.
//! Template: apps/shared_libs/compress/build.zig.

const std = @import("std");

/// The vendored libwebp decode-only subset, every TU except utils/utils.c,
/// which carries the arena patch flag and compiles separately below.
const vendor_sources = [_][]const u8{
    "dec/alpha_dec.c",
    "dec/buffer_dec.c",
    "dec/frame_dec.c",
    "dec/idec_dec.c",
    "dec/io_dec.c",
    "dec/quant_dec.c",
    "dec/tree_dec.c",
    "dec/vp8_dec.c",
    "dec/vp8l_dec.c",
    "dec/webp_dec.c",
    "dsp/alpha_processing.c",
    "dsp/alpha_processing_mips_dsp_r2.c",
    "dsp/alpha_processing_neon.c",
    "dsp/alpha_processing_sse2.c",
    "dsp/alpha_processing_sse41.c",
    "dsp/cpu.c",
    "dsp/dec.c",
    "dsp/dec_clip_tables.c",
    "dsp/dec_mips32.c",
    "dsp/dec_mips_dsp_r2.c",
    "dsp/dec_msa.c",
    "dsp/dec_neon.c",
    "dsp/dec_sse2.c",
    "dsp/dec_sse41.c",
    "dsp/filters.c",
    "dsp/filters_mips_dsp_r2.c",
    "dsp/filters_msa.c",
    "dsp/filters_neon.c",
    "dsp/filters_sse2.c",
    "dsp/lossless.c",
    "dsp/lossless_mips_dsp_r2.c",
    "dsp/lossless_msa.c",
    "dsp/lossless_neon.c",
    "dsp/lossless_sse2.c",
    "dsp/lossless_sse41.c",
    "dsp/rescaler.c",
    "dsp/rescaler_mips32.c",
    "dsp/rescaler_mips_dsp_r2.c",
    "dsp/rescaler_msa.c",
    "dsp/rescaler_neon.c",
    "dsp/rescaler_sse2.c",
    "dsp/upsampling.c",
    "dsp/upsampling_mips_dsp_r2.c",
    "dsp/upsampling_msa.c",
    "dsp/upsampling_neon.c",
    "dsp/upsampling_sse2.c",
    "dsp/upsampling_sse41.c",
    "dsp/yuv.c",
    "dsp/yuv_mips32.c",
    "dsp/yuv_mips_dsp_r2.c",
    "dsp/yuv_neon.c",
    "dsp/yuv_sse2.c",
    "dsp/yuv_sse41.c",
    "utils/bit_reader_utils.c",
    "utils/color_cache_utils.c",
    "utils/filters_utils.c",
    "utils/huffman_utils.c",
    "utils/palette.c",
    "utils/quant_levels_dec_utils.c",
    "utils/random_utils.c",
    "utils/rescaler_utils.c",
    "utils/thread_utils.c",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_webp_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "webp_facade",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig WebP facade tests");
    const test_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_webp_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.link_libc = true;
    test_module.addIncludePath(b.path("inc"));
    test_module.addIncludePath(b.path("../../../libs/ra8_core/inc"));
    test_module.addIncludePath(b.path("../../../libs/ra8_imgdec/inc"));
    test_module.addIncludePath(b.path("../../../libs/ra8_mem/inc"));
    test_module.addIncludePath(b.path("../third_party/libwebp"));

    const webp_test = b.addTest(.{ .root_module = test_module });

    // The vendored libwebp decoder (SOUP): the archive the facade's externs
    // resolve against. utils.c compiles with the arena patch flag so libwebp's
    // WebPSafeMalloc/Calloc/Free route through the Zig hooks above, which is
    // what makes the package test a zero-heap proof rather than a decode smoke.
    test_module.addCSourceFiles(.{
        .root = b.path("../third_party/libwebp/src"),
        .files = &vendor_sources,
    });
    test_module.addCSourceFiles(.{
        .root = b.path("../third_party/libwebp/src"),
        .files = &.{"utils/utils.c"},
        .flags = &.{"-DRA8_WEBP_USE_ARENA"},
    });

    // The scratch policy the arena hooks forward to (RA8FW-308) and the log
    // backend the facade's error lines go through, both Zig archives.
    test_module.linkLibrary(b.dependency("ra8_imgdec", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_imgdec"));
    // ra8_core publishes the split host archive (ra8_core_zig) and the single
    // composed image archive on freestanding targets, so the artifact name
    // follows the target the way ra8_core's own build does.
    const core_artifact = if (target.result.os.tag == .freestanding) "ra8_core" else "ra8_core_zig";
    test_module.linkLibrary(b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact(core_artifact));
    // And the bump arena the scratch carves from: ra8_mem_abi.zig's exports
    // (ra8_arena_carve / ra8_arena_remaining) resolve here.
    test_module.linkLibrary(b.dependency("ra8_mem", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_mem"));

    test_step.dependOn(&b.addRunArtifact(webp_test).step);
}
