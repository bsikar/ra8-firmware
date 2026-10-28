//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! tools/rabook_viewer -- native reader viewer (direct-call, not an emulator).
//!
//!   zig build --prefix build
//!   ./build/bin/rabook_viewer book.jof --headless --dump-ppm page0.ppm
//!
//! RA8FW-999: built through the Zig graph like every other host tool; the
//! C-era build linked the firmware's reader libraries as one ra8_reader
//! archive (comic, longstrip, the JOF read path, the SVG reflow read path,
//! unarch's tar members, epub's miniz allocator, the compression seam) and
//! took ra8_imgdec, ra8_gfx, ra8_mem, ra8_core and ra8_io as Zig archives.
//! The WebP decode stack rides the shared recipe cmake/ra8_webp_vendor.cmake
//! carries; the reader's image path calls it from reflow_image.c.
//!
//! The Cocoa window backend is macOS-only; off Apple the build compiles the
//! headless stub (src/ra8_viewer_view_stub.c), exactly the C-era split.

const std = @import("std");

// First-party firmware sources carry the project warning bar (NASA Power of
// 10 Rule 10) exactly as they do in the ARM and host-test builds. The reader
// archive's definitions are PUBLIC in the C-era target (MINIZ carve-downs and
// RA8_OFF_TARGET), so consumers inherit them; they ride the flag list here.
const first_party_flags = [_][]const u8{
    "-std=gnu23",         "-Wall",
    "-Wextra",            "-Werror",
    "-DRA8_OFF_TARGET",   "-DMINIZ_NO_MALLOC",
    "-DMINIZ_NO_STDIO",   "-DMINIZ_NO_TIME",
};

// miniz and the stb image residue type-pun through byte buffers; the C-era
// source-file properties kept exactly -Wno-unused-parameter (the two unused
// callback parameters) plus strict aliasing off, nothing demoted.
const third_party_flags = [_][]const u8{
    "-std=gnu23",            "-Wall",
    "-Wextra",               "-Werror",
    "-Wno-unused-parameter", "-fno-strict-aliasing",
    "-DRA8_OFF_TARGET",      "-DMINIZ_NO_MALLOC",
    "-DMINIZ_NO_STDIO",      "-DMINIZ_NO_TIME",
};

// Part 3 + 4 of the webp recipe (cmake/ra8_webp_vendor.cmake): the arena
// define is load-bearing for the zero-heap property, and the 60+ TU codec is
// compiled quietly with strict aliasing off.
const webp_vendor_flags = [_][]const u8{
    "-std=gnu23",           "-w",
    "-fno-strict-aliasing", "-DRA8_WEBP_USE_ARENA",
    "-DRA8_OFF_TARGET",     "-DMINIZ_NO_MALLOC",
    "-DMINIZ_NO_STDIO",     "-DMINIZ_NO_TIME",
};

const comic_dir = "../../apps/shared_libs/comic/src/";
const longstrip_dir = "../../apps/shared_libs/longstrip/src/";
const epub_dir = "../../apps/shared_libs/epub/src/";
const reflow_dir = "../../apps/shared_libs/reflow/src/";
const jof_dir = "../../apps/shared_libs/jof/src/";
const compress_dir = "../../apps/shared_libs/compress/src/";

// The read-path members, derived the way the C-era recipe derived them.
// Whole-read-path libraries are their full src/ glob (comic, longstrip);
// partial libraries name exactly the KEEP members the C-era
// ra8_viewer_read_path() partition proved (jof, epub, reflow, unarch,
// compress). libs/ra8_io/src holds no .c left (both block-device members
// were the last, and they are DROP), so its read path comes out of the
// ra8_io archive like the C-era link already did.
const comic_closure = [_][]const u8{
    comic_dir ++ "comic.c",
    comic_dir ++ "comic_cbr.c",
    comic_dir ++ "comic_cbt.c",
    comic_dir ++ "comic_cbz.c",
    comic_dir ++ "comic_tiles.c",
    comic_dir ++ "comic_wrapped.c",
    comic_dir ++ "ra8_rar.c",
    comic_dir ++ "ra8_rar5.c",
    comic_dir ++ "ra8_rar5_tables.c",
};
const longstrip_closure = [_][]const u8{longstrip_dir ++ "longstrip.c"};
const epub_closure = [_][]const u8{epub_dir ++ "epub_miniz_alloc.c"};
const reflow_closure = [_][]const u8{
    reflow_dir ++ "ra8_img_arena.c",
    reflow_dir ++ "reflow_image.c",
    reflow_dir ++ "reflow_svg.c",
    reflow_dir ++ "reflow_svg_doc.c",
    reflow_dir ++ "reflow_svg_path.c",
    reflow_dir ++ "reflow_svg_shape.c",
    reflow_dir ++ "reflow_svg_xform.c",
};
const jof_closure = [_][]const u8{jof_dir ++ "jof.c"};
const compress_closure = [_][]const u8{compress_dir ++ "ra8_compress.c"};
const unarch_closure = [_][]const u8{
    "../../apps/shared_libs/unarch/src/unarch_io.c",
    "../../apps/shared_libs/unarch/src/unarch_tar.c",
    "../../apps/shared_libs/unarch/src/unarch_tar_fields.c",
};

const miniz_sources = [_][]const u8{"../../apps/shared_libs/third_party/miniz/miniz.c"};
const stb_sources = [_][]const u8{"../../apps/shared_libs/third_party/stb/stb_image_impl.c"};

// The facade is not self-contained: ra8_webp_imgdec.c calls ra8_imgdec_dims()
// and ra8_imgdec_pixel_bytes(), and those call ra8_imgdec_sniff(). All three
// live in the ra8_imgdec archive, so the archive completes the closure the
// way the C-era link did.
const webp_facade_sources = [_][]const u8{
    "../../apps/shared_libs/webp/src/ra8_webp.c",
    "../../apps/shared_libs/webp/src/ra8_webp_arena.c",
    "../../apps/shared_libs/webp/src/ra8_webp_imgdec.c",
};

// The vendored decode-only TU set (the C-era GLOB_RECURSE over
// apps/shared_libs/third_party/libwebp/src, written out statically so
// the build needs no filesystem walk at configure time).
const webp_vendor_sources = [_][]const u8{
    "../../apps/shared_libs/third_party/libwebp/src/dec/alpha_dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dec/buffer_dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dec/frame_dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dec/idec_dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dec/io_dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dec/quant_dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dec/tree_dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dec/vp8_dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dec/vp8l_dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dec/webp_dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/alpha_processing.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/alpha_processing_mips_dsp_r2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/alpha_processing_neon.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/alpha_processing_sse2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/alpha_processing_sse41.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/cpu.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/dec.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/dec_clip_tables.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/dec_mips32.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/dec_mips_dsp_r2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/dec_msa.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/dec_neon.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/dec_sse2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/dec_sse41.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/filters.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/filters_mips_dsp_r2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/filters_msa.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/filters_neon.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/filters_sse2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/lossless.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/lossless_mips_dsp_r2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/lossless_msa.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/lossless_neon.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/lossless_sse2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/lossless_sse41.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/rescaler.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/rescaler_mips32.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/rescaler_mips_dsp_r2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/rescaler_msa.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/rescaler_neon.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/rescaler_sse2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/upsampling.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/upsampling_mips_dsp_r2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/upsampling_msa.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/upsampling_neon.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/upsampling_sse2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/upsampling_sse41.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/yuv.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/yuv_mips32.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/yuv_mips_dsp_r2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/yuv_neon.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/yuv_sse2.c",
    "../../apps/shared_libs/third_party/libwebp/src/dsp/yuv_sse41.c",
    "../../apps/shared_libs/third_party/libwebp/src/utils/bit_reader_utils.c",
    "../../apps/shared_libs/third_party/libwebp/src/utils/color_cache_utils.c",
    "../../apps/shared_libs/third_party/libwebp/src/utils/filters_utils.c",
    "../../apps/shared_libs/third_party/libwebp/src/utils/huffman_utils.c",
    "../../apps/shared_libs/third_party/libwebp/src/utils/palette.c",
    "../../apps/shared_libs/third_party/libwebp/src/utils/quant_levels_dec_utils.c",
    "../../apps/shared_libs/third_party/libwebp/src/utils/random_utils.c",
    "../../apps/shared_libs/third_party/libwebp/src/utils/rescaler_utils.c",
    "../../apps/shared_libs/third_party/libwebp/src/utils/thread_utils.c",
    "../../apps/shared_libs/third_party/libwebp/src/utils/utils.c",
};

fn addC(b: *std.Build, mod: *std.Build.Module, root: []const u8, files: []const []const u8, flags: []const []const u8) void {
    mod.addCSourceFiles(.{ .root = b.path(root), .files = files, .flags = flags });
}

fn addWebp(b: *std.Build, mod: *std.Build.Module) void {
    addC(b, mod, ".", &webp_facade_sources, &first_party_flags);
    mod.addCSourceFiles(.{ .root = b.path("."), .files = &webp_vendor_sources, .flags = &webp_vendor_flags });
}

fn addWebpIncludes(b: *std.Build, mod: *std.Build.Module) void {
    mod.addIncludePath(b.path("../../apps/shared_libs/third_party/libwebp"));
    mod.addIncludePath(b.path("../../apps/shared_libs/webp/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/webp/src"));
}

// The ra8_reader PUBLIC include set from the C-era target, which every
// consumer of the archive inherited.
fn addReaderIncludes(b: *std.Build, mod: *std.Build.Module) void {
    mod.addIncludePath(b.path("../../libs/ra8_core/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/comic/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/epub/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_gfx/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/longstrip/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/jof/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_io/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/compress/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_mem/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/reflow/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_imgdec/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/unarch/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/third_party/miniz"));
    mod.addIncludePath(b.path("../../apps/shared_libs/third_party/stb"));
    addWebpIncludes(b, mod);
}

// The Zig archives the C-era recipe linked instead of compiling those
// libraries by path.
fn linkZigLibs(b: *std.Build, mod: *std.Build.Module, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) void {
    const imgdec_lib = b.dependency("ra8_imgdec", .{ .target = target, .optimize = optimize }).artifact("ra8_imgdec");
    const gfx_lib = b.dependency("ra8_gfx", .{ .target = target, .optimize = optimize }).artifact("ra8_gfx");
    const mem_lib = b.dependency("ra8_mem", .{ .target = target, .optimize = optimize }).artifact("ra8_mem");
    const core_lib = b.dependency("ra8_core", .{ .target = target, .optimize = optimize }).artifact("ra8_core_zig");
    // The viewer needs only the stream units: its read path drives the ra8_io
    // stream ABI, and the one KEEP/DROP member that wanted more (epub_fs.c) is
    // DROP. The narrow root is the same units=stream archive glyph_bench uses.
    const io_lib = b.dependency("ra8_io", .{ .target = target, .optimize = optimize, .units = "stream" }).artifact("ra8_io");
    mod.linkLibrary(imgdec_lib);
    mod.linkLibrary(gfx_lib);
    mod.linkLibrary(mem_lib);
    mod.linkLibrary(core_lib);
    mod.linkLibrary(io_lib);
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    // The viewer rasterises real pages, so the default build optimizes like
    // the C-era tool build did; an explicit -Doptimize still wins.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    // ---- ra8_reader: the archive of firmware read-path sources ----
    const reader_mod = b.createModule(.{ .target = target, .optimize = optimize });
    reader_mod.link_libc = true;
    linkZigLibs(b, reader_mod, target, optimize);
    addReaderIncludes(b, reader_mod);
    addC(b, reader_mod, ".", &comic_closure, &first_party_flags);
    addC(b, reader_mod, ".", &longstrip_closure, &first_party_flags);
    addC(b, reader_mod, ".", &epub_closure, &first_party_flags);
    addC(b, reader_mod, ".", &reflow_closure, &first_party_flags);
    addC(b, reader_mod, ".", &jof_closure, &first_party_flags);
    addC(b, reader_mod, ".", &compress_closure, &first_party_flags);
    addC(b, reader_mod, ".", &unarch_closure, &first_party_flags);
    reader_mod.addCSourceFiles(.{ .root = b.path("."), .files = &miniz_sources, .flags = &third_party_flags });
    reader_mod.addCSourceFiles(.{ .root = b.path("."), .files = &stb_sources, .flags = &third_party_flags });
    addWebp(b, reader_mod);
    const reader = b.addLibrary(.{ .name = "ra8_reader", .linkage = .static, .root_module = reader_mod });

    // ---- rabook_viewer: C core + window backend ----
    // Off Apple the headless stub supplies the identical C ABI and reports
    // "no window" so main.c takes its existing headless branch; exactly one
    // of the two backends is ever compiled.
    const exe_sources = [_][]const u8{
        "src/main.c",
        "src/ra8_viewer_output.c",
        "src/ra8_viewer_reader.c",
        "src/ra8_viewer_comic.c",
        "src/ra8_viewer_jof.c",
        "src/ra8_viewer_ppm.c",
        "../../port/posix/src/ra8_io_stream_posix.c",
        "src/ra8_viewer_view_stub.c",
    };
    const exe_mod = b.createModule(.{ .target = target, .optimize = optimize });
    exe_mod.link_libc = true;
    linkZigLibs(b, exe_mod, target, optimize);
    exe_mod.addIncludePath(b.path("inc"));
    exe_mod.addIncludePath(b.path("../../port/posix/inc"));
    addReaderIncludes(b, exe_mod);
    addC(b, exe_mod, ".", &exe_sources, &first_party_flags);
    exe_mod.linkLibrary(reader);
    const exe = b.addExecutable(.{ .name = "rabook_viewer", .root_module = exe_mod });
    b.installArtifact(exe);

    // ---- unit targets (the C-era add_test set) ----
    const Unit = struct { name: []const u8, files: []const []const u8, extra_includes: []const []const u8 };

    const units = [_]Unit{
        .{
            .name = "test_viewer_workspace",
            .files = &.{
                "tests/src/test_viewer_workspace.c",
                "src/ra8_viewer_reader.c",
                "src/ra8_viewer_comic.c",
                "src/ra8_viewer_jof.c",
                "src/ra8_viewer_ppm.c",
            },
            .extra_includes = &.{"inc"},
        },
        .{
            .name = "test_viewer_comic_workspace",
            .files = &.{
                "tests/src/test_viewer_comic_workspace.c",
                "src/ra8_viewer_reader.c",
                "src/ra8_viewer_comic.c",
                "src/ra8_viewer_jof.c",
                "src/ra8_viewer_ppm.c",
            },
            .extra_includes = &.{"inc"},
        },
        .{
            .name = "test_viewer_output",
            .files = &.{
                "tests/src/test_viewer_output.c",
                "src/ra8_viewer_output.c",
                "../../port/posix/src/ra8_io_stream_posix.c",
            },
            .extra_includes = &.{ "inc", "../../port/posix/inc" },
        },
    };

    const test_step = b.step("test", "Run the rabook_viewer unit tests");
    var test_exes: [units.len]*std.Build.Step.Compile = undefined;
    for (units, 0..) |unit, i| {
        const tmod = b.createModule(.{ .target = target, .optimize = optimize });
        tmod.link_libc = true;
        linkZigLibs(b, tmod, target, optimize);
        addReaderIncludes(b, tmod);
        for (unit.extra_includes) |inc_path| tmod.addIncludePath(b.path(inc_path));
        addC(b, tmod, ".", unit.files, &first_party_flags);
        tmod.linkLibrary(reader);
        const test_exe = b.addExecutable(.{ .name = unit.name, .root_module = tmod });
        test_exes[i] = test_exe;
        b.installArtifact(test_exe);
    }
    // The C-era add_test set never ran the two workspace tests bare:
    // run_workspace_test.sh drives test_viewer_workspace against a scratch
    // workdir, and test_viewer_comic_workspace takes its fixture CBig on
    // argv. Only test_viewer_output is self-contained.
    test_step.dependOn(&b.addRunArtifact(test_exes[2]).step);
    const comic_test_run = b.addRunArtifact(test_exes[1]);
    comic_test_run.addArg("fixtures/sample.cbz");
    test_step.dependOn(&comic_test_run.step);

    // ---- the C-era integration scripts, driven against the built exes ----
    const integration_step = b.step("integration", "Run the rabook_viewer integration suites");
    {
        const cmd = b.addSystemCommand(&.{ "bash", "tests/run_workspace_test.sh" });
        cmd.addArtifactArg(test_exes[0]);
        cmd.addArg("workspace_test");
        integration_step.dependOn(&cmd.step);
    }
    {
        const cmd = b.addSystemCommand(&.{ "bash", "tests/run_corpus.sh" });
        cmd.addArtifactArg(exe);
        integration_step.dependOn(&cmd.step);
    }
}
