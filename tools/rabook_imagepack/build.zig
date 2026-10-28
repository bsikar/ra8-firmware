//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! tools/rabook_imagepack -- host build of the single-file converter /
//! inspector for the first-party content container formats.
//!
//!   zig build --prefix build
//!   ./build/bin/rabook_imagepack convert --format jof --in page.jpg --out page.jof
//!
//! RA8FW-999: built through the Zig graph like every other host tool; the
//! C-era CMake build linked the same Zig archives (ra8_imgdec, ra8_mem,
//! ra8_jpeg, ra8_core) instead of compiling those libraries by path, carried
//! the WebP decode stack through cmake/ra8_webp_vendor.cmake, and ran the
//! six unit targets plus the four integration scripts through CTest.

const std = @import("std");

// First-party sources carry the project warning bar (NASA Power of 10 Rule
// 10) exactly as they do in the ARM and host-test builds; only vendored SOUP
// is compiled quietly. The feature-macro contract (_GNU_SOURCE) and the
// miniz carve-down definitions come from the C-era target-wide definitions.
const first_party_flags = [_][]const u8{
    "-std=gnu23",              "-Wall",
    "-Wextra",                 "-Werror",
    "-DRA8_OFF_TARGET",        "-D_GNU_SOURCE",
    "-DMINIZ_NO_ARCHIVE_APIS", "-DMINIZ_NO_MALLOC",
    "-DMINIZ_NO_STDIO",        "-DMINIZ_NO_TIME",
};

// miniz type-puns through byte buffers; disable strict aliasing to prevent
// the corresponding miscompile without demoting unrelated diagnostics.
const miniz_flags = [_][]const u8{
    "-std=gnu23",              "-Wall",
    "-Wextra",                 "-Werror",
    "-fno-strict-aliasing",    "-DRA8_OFF_TARGET",
    "-D_GNU_SOURCE",           "-DMINIZ_NO_ARCHIVE_APIS",
    "-DMINIZ_NO_MALLOC",       "-DMINIZ_NO_STDIO",
    "-DMINIZ_NO_TIME",
};

// Part 3 + 4 of the webp recipe (cmake/ra8_webp_vendor.cmake): the arena
// define is load-bearing for the zero-heap property, and the 60+ TU codec is
// compiled quietly with strict aliasing off.
const webp_vendor_flags = [_][]const u8{
    "-std=gnu23",              "-w",
    "-fno-strict-aliasing",    "-DRA8_WEBP_USE_ARENA",
    "-DRA8_OFF_TARGET",        "-D_GNU_SOURCE",
    "-DMINIZ_NO_ARCHIVE_APIS", "-DMINIZ_NO_MALLOC",
    "-DMINIZ_NO_STDIO",        "-DMINIZ_NO_TIME",
};

const jof_dir = "../../apps/shared_libs/jof/src/";
const book_dir = "../../apps/shared_libs/book/src/";
const compress_dir = "../../apps/shared_libs/compress/src/";
const rabook_compile_dir = "../../apps/shared_libs/rabook_compile/src/";

// The exact firmware members the tool calls: the JOF producer (with its
// JPEG / PNG / WebP front ends) and reader, plus the compression seam they
// sit on. Naming members rather than globbing keeps the HAL and peripheral
// layers out of a host tool. The JPEG codec is Zig: RA8_FMT_JPEG_SRC is
// empty in the C-era recipe and stays empty here.
const jof_closure = [_][]const u8{
    jof_dir ++ "jof.c",
    jof_dir ++ "jof_produce.c",
    jof_dir ++ "jof_produce_webp.c",
    jof_dir ++ "jof_png.c",
    jof_dir ++ "jof_png_chunk.c",
    compress_dir ++ "ra8_compress.c",
};

const book_closure = [_][]const u8{
    book_dir ++ "book.c",
    book_dir ++ "book_chunked.c",
    book_dir ++ "book_chunked_validate.c",
    book_dir ++ "book_stream.c",
    book_dir ++ "book_stream_wire.c",
};

const miniz_sources = [_][]const u8{"../../apps/shared_libs/third_party/miniz/miniz.c"};

// The facade is not self-contained: ra8_webp_imgdec.c calls ra8_imgdec_dims()
// and ra8_imgdec_pixel_bytes(), and those call ra8_imgdec_sniff(). All three
// live in the ra8_imgdec archive, so the archive completes the closure the
// way the C-era link did.
const webp_facade_sources = [_][]const u8{
    "../../apps/shared_libs/webp/src/ra8_webp.c",
    "../../apps/shared_libs/webp/src/ra8_webp_arena.c",
    "../../apps/shared_libs/webp/src/ra8_webp_imgdec.c",
};

fn addC(b: *std.Build, mod: *std.Build.Module, root: []const u8, files: []const []const u8, flags: []const []const u8) void {
    mod.addCSourceFiles(.{ .root = b.path(root), .files = files, .flags = flags });
}

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

fn addWebp(b: *std.Build, mod: *std.Build.Module) void {
    addC(b, mod, ".", &webp_facade_sources, &first_party_flags);
    mod.addCSourceFiles(.{ .root = b.path("."), .files = &webp_vendor_sources, .flags = &webp_vendor_flags });
}

fn addWebpIncludes(b: *std.Build, mod: *std.Build.Module) void {
    mod.addIncludePath(b.path("../../apps/shared_libs/third_party/libwebp"));
    mod.addIncludePath(b.path("../../apps/shared_libs/webp/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/webp/src"));
}

fn addToolIncludes(b: *std.Build, mod: *std.Build.Module) void {
    mod.addIncludePath(b.path("inc"));
    mod.addIncludePath(b.path("../../libs/ra8_core/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_fmt/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/jof/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_jpeg/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_jpeg/src"));
    mod.addIncludePath(b.path("../../apps/shared_libs/jof/src"));
    mod.addIncludePath(b.path("../../libs/ra8_io/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/compress/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_imgdec/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/book/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_mem/inc"));
    mod.addIncludePath(b.path("../../apps/shared_libs/third_party/miniz"));
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    // The converter transcodes real images, so the default build optimizes
    // like the C-era tool build did; an explicit -Doptimize still wins.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    // The libraries the C-era recipe linked as Zig archives.
    const imgdec_lib = b.dependency("ra8_imgdec", .{ .target = target, .optimize = optimize }).artifact("ra8_imgdec");
    const mem_lib = b.dependency("ra8_mem", .{ .target = target, .optimize = optimize }).artifact("ra8_mem");
    const jpeg_lib = b.dependency("ra8_jpeg", .{ .target = target, .optimize = optimize }).artifact("ra8_jpeg");
    const core_lib = b.dependency("ra8_core", .{ .target = target, .optimize = optimize }).artifact("ra8_core_zig");

    const tool_sources = [_][]const u8{
        "src/main.c",
        "src/ra8_fmt_stream_convert.c",
        "src/ra8_fmt_stream_inspect.c",
        "src/ra8_fmt_stream_rabook.c",
        "src/ra8_fmt_stream_verify.c",
        "src/ra8_fmt_stream_verify_plan.c",
        "src/ra8_fmt_host_fd.c",
        "src/ra8_fmt_host_spool.c",
        "src/ra8_fmt_portable_convert.c",
        "src/ra8_fmt_portable_verify.c",
        "src/ra8_fmt_portable_main.c",
        jof_dir ++ "jof_audit.c",
    };

    const mod = b.createModule(.{ .target = target, .optimize = optimize });
    mod.link_libc = true;
    mod.linkLibrary(imgdec_lib);
    mod.linkLibrary(mem_lib);
    mod.linkLibrary(jpeg_lib);
    mod.linkLibrary(core_lib);
    addToolIncludes(b, mod);
    addWebpIncludes(b, mod);
    addC(b, mod, ".", &tool_sources, &first_party_flags);
    addC(b, mod, ".", &jof_closure, &first_party_flags);
    addC(b, mod, ".", &book_closure, &first_party_flags);
    mod.addCSourceFiles(.{ .root = b.path("."), .files = &miniz_sources, .flags = &miniz_flags });
    addWebp(b, mod);
    const exe = b.addExecutable(.{ .name = "rabook_imagepack", .root_module = mod });
    b.installArtifact(exe);

    // ---- unit targets (the C-era add_test set) ----
    const Unit = struct { name: []const u8, files: []const []const u8, includes: []const []const u8, webp: bool, jofc: bool = false, bookc: bool = false, extra_defs: []const []const u8 = &.{} };

    const support_inc = "../../tests/support/inc";
    const miniz_inc = "../../apps/shared_libs/third_party/miniz";
    const units = [_]Unit{
        .{
            .name = "test_jof_audit_portable",
            .files = &.{
                "../../apps/shared_libs/jof/tests/src/test_jof_audit.c",
                jof_dir ++ "jof_audit.c",
                jof_dir ++ "jof.c",
                compress_dir ++ "ra8_compress.c",
            },
            .includes = &.{ support_inc, "inc", "../../libs/ra8_core/inc", "../../libs/ra8_fmt/inc", "../../apps/shared_libs/jof/inc", "../../libs/ra8_io/inc", "../../apps/shared_libs/compress/inc", miniz_inc },
            .webp = false,
        },
        .{
            .name = "test_ra8_fmt_stream_inspect",
            .files = &.{
                "tests/src/test_ra8_fmt_stream_inspect.c",
                "src/ra8_fmt_stream_inspect.c",
                jof_dir ++ "jof_audit.c",
                jof_dir ++ "jof.c",
                compress_dir ++ "ra8_compress.c",
            },
            .includes = &.{ "inc", "../../libs/ra8_core/inc", "../../libs/ra8_fmt/inc", "../../apps/shared_libs/jof/inc", "../../libs/ra8_io/inc", "../../apps/shared_libs/compress/inc", miniz_inc },
            .webp = false,
        },
        .{
            .name = "test_ra8_fmt_host_fd",
            .files = &.{ "tests/src/test_ra8_fmt_host_fd.c", "src/ra8_fmt_host_fd.c" },
            .includes = &.{ "inc", "../../libs/ra8_core/inc", "../../libs/ra8_fmt/inc", "../../apps/shared_libs/jof/inc" },
            .webp = false,
        },
        .{
            .name = "test_ra8_fmt_stream_convert",
            .files = &.{ "tests/src/test_ra8_fmt_stream_convert.c", "src/ra8_fmt_stream_convert.c", "src/ra8_fmt_host_fd.c" },
            .includes = &.{ "inc", "../../libs/ra8_core/inc", "../../libs/ra8_fmt/inc", "../../apps/shared_libs/jof/inc", "../../libs/ra8_jpeg/inc", "../../libs/ra8_jpeg/src", "../../apps/shared_libs/jof/src", "../../libs/ra8_io/inc", "../../apps/shared_libs/compress/inc", "../../libs/ra8_imgdec/inc", "../../libs/ra8_mem/inc", miniz_inc },
            .webp = true,
            .jofc = true,
            .extra_defs = &.{"-DRA8_FMT_TEST_PNG=\"../../tests/fixtures/rabook_fixed_layout/OEBPS/images/page1.png\""},
        },
        .{
            .name = "test_ra8_fmt_stream_verify",
            .files = &.{ "tests/src/test_ra8_fmt_stream_verify.c", "src/ra8_fmt_stream_verify.c", "src/ra8_fmt_stream_verify_plan.c", "src/ra8_fmt_stream_convert.c" },
            .includes = &.{ "inc", "../../libs/ra8_core/inc", "../../libs/ra8_fmt/inc", "../../apps/shared_libs/jof/inc", "../../libs/ra8_jpeg/inc", "../../libs/ra8_jpeg/src", "../../apps/shared_libs/jof/src", "../../libs/ra8_io/inc", "../../apps/shared_libs/compress/inc", "../../libs/ra8_imgdec/inc", "../../libs/ra8_mem/inc", miniz_inc },
            .webp = true,
            .jofc = true,
            .extra_defs = &.{"-DRA8_FMT_TEST_PNG=\"../../tests/fixtures/rabook_fixed_layout/OEBPS/images/page1.png\""},
        },
        .{
            .name = "test_ra8_fmt_stream_rabook",
            .files = &.{
                "tests/src/test_ra8_fmt_stream_rabook.c",
                "src/ra8_fmt_stream_rabook.c",
                "../../apps/shared_libs/rabook_compile/tests/src/rabook_compile_test_fixture.c",
                rabook_compile_dir ++ "rabook_compile.c",
                rabook_compile_dir ++ "ra8_rabook_finalize.c",
                rabook_compile_dir ++ "ra8_rabook_container.c",
                compress_dir ++ "ra8_compress.c",
            },
            .includes = &.{ support_inc, "../../apps/shared_libs/rabook_compile/tests/inc", "inc", "../../libs/ra8_core/inc", "../../libs/ra8_fmt/inc", "../../apps/shared_libs/book/inc", "../../apps/shared_libs/book/src", "../../libs/ra8_io/inc", "../../apps/shared_libs/compress/inc", "../../apps/shared_libs/jof/inc", "../../libs/ra8_mem/inc", "../../apps/shared_libs/rabook_compile/inc", miniz_inc },
            .webp = false,
            .bookc = true,
        },
    };

    const test_step = b.step("test", "Run the rabook_imagepack unit tests");
    var test_exes: [units.len]*std.Build.Step.Compile = undefined;
    for (units, 0..) |unit, i| {
        const tmod = b.createModule(.{ .target = target, .optimize = optimize });
        tmod.link_libc = true;
        tmod.linkLibrary(imgdec_lib);
        tmod.linkLibrary(mem_lib);
        tmod.linkLibrary(jpeg_lib);
        tmod.linkLibrary(core_lib);
        for (unit.includes) |inc_path| tmod.addIncludePath(b.path(inc_path));
        // The C-era test targets carried extra definitions per target
        // (RA8_FMT_TEST_PNG's golden fixture path); those ride on the unit's
        // own TUs, so their flag list is the warning bar plus the extras.
        if (unit.extra_defs.len > 0) {
            var unit_flags: std.ArrayList([]const u8) = .empty;
            unit_flags.appendSlice(b.allocator, &first_party_flags) catch @panic("oom");
            unit_flags.appendSlice(b.allocator, unit.extra_defs) catch @panic("oom");
            addC(b, tmod, ".", unit.files, unit_flags.items);
        } else {
            addC(b, tmod, ".", unit.files, &first_party_flags);
        }
        if (unit.jofc) addC(b, tmod, ".", &jof_closure, &first_party_flags);
        if (unit.bookc) addC(b, tmod, ".", &book_closure, &first_party_flags);
        tmod.addCSourceFiles(.{ .root = b.path("."), .files = &miniz_sources, .flags = &miniz_flags });
        if (unit.webp) {
            addWebpIncludes(b, tmod);
            addWebp(b, tmod);
        }
        const test_exe = b.addExecutable(.{ .name = unit.name, .root_module = tmod });
        test_exes[i] = test_exe;
        b.installArtifact(test_exe);
        test_step.dependOn(&b.addRunArtifact(test_exe).step);
    }
    _ = &test_exes;

    // ---- the four C-era integration scripts, driven against the built exes ----
    const integration_step = b.step("integration", "Run the rabook_imagepack integration suites");
    {
        const cmd = b.addSystemCommand(&.{ "sh", "tests/rabook_inspect_integration.sh" });
        cmd.addArtifactArg(exe);
        cmd.addArtifactArg(test_exes[5]);
        integration_step.dependOn(&cmd.step);
    }
    {
        const cmd = b.addSystemCommand(&.{ "sh", "tests/convert_integration.sh" });
        cmd.addArtifactArg(exe);
        cmd.addArg("../..");
        integration_step.dependOn(&cmd.step);
    }
    {
        const cmd = b.addSystemCommand(&.{ "sh", "tests/verify_integration.sh" });
        cmd.addArtifactArg(exe);
        cmd.addArg("../..");
        integration_step.dependOn(&cmd.step);
    }
    {
        const cmd = b.addSystemCommand(&.{ "sh", "tests/check_production_surface.sh" });
        cmd.addArg(".");
        cmd.addArtifactArg(exe);
        integration_step.dependOn(&cmd.step);
    }
}
