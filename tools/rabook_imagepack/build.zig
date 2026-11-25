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
const ra8_build = @import("ra8_zig_build");

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

fn addC(b: *std.Build, mod: *std.Build.Module, root: []const u8, files: []const []const u8, flags: []const []const u8) void {
    mod.addCSourceFiles(.{ .root = b.path(root), .files = files, .flags = flags });
}

const Deps = struct {
    fmt_lib: *std.Build.Step.Compile,
    webp_lib: *std.Build.Step.Compile,
    webp_vendor_lib: *std.Build.Step.Compile,
    imgdec_lib: *std.Build.Step.Compile,
    mem_lib: *std.Build.Step.Compile,
    jpeg_lib: *std.Build.Step.Compile,
    core_lib: *std.Build.Step.Compile,
    jof_lib: *std.Build.Step.Compile,
    jof_c_tests: std.Build.LazyPath,
    compress_lib: *std.Build.Step.Compile,
    miniz_lib: *std.Build.Step.Compile,
    miniz_host_lib: *std.Build.Step.Compile,
    book_lib: *std.Build.Step.Compile,
    io_include_dir: std.Build.LazyPath,
    reflow_include_dir: std.Build.LazyPath,
    stb_image_lib: *std.Build.Step.Compile,
    rabook_compile_lib: *std.Build.Step.Compile,
    rabook_compile_fixture_lib: *std.Build.Step.Compile,
};

fn linkEngineDeps(
    b: *std.Build,
    mod: *std.Build.Module,
    deps: Deps,
    with_webp: bool,
    with_malloc_miniz: bool,
    with_stb_image: bool,
    with_rabook: bool,
) void {
    mod.addIncludePath(b.path("inc"));
    mod.addIncludePath(deps.io_include_dir);
    if (with_stb_image) {
        mod.addIncludePath(deps.reflow_include_dir);
        mod.linkLibrary(deps.stb_image_lib);
    }
    if (with_rabook) {
        mod.linkLibrary(deps.rabook_compile_fixture_lib);
        mod.linkLibrary(deps.rabook_compile_lib);
    }
    mod.linkLibrary(deps.fmt_lib);
    if (with_webp) {
        mod.linkLibrary(deps.webp_lib);
        mod.linkLibrary(deps.webp_vendor_lib);
    } else {
        mod.addIncludePath(deps.webp_lib.getEmittedIncludeTree());
    }
    mod.linkLibrary(deps.imgdec_lib);
    mod.linkLibrary(deps.jpeg_lib);
    mod.linkLibrary(deps.jof_lib);
    mod.linkLibrary(deps.compress_lib);
    mod.linkLibrary(if (with_malloc_miniz) deps.miniz_host_lib else deps.miniz_lib);
    mod.linkLibrary(deps.book_lib);
    mod.linkLibrary(deps.mem_lib);
    mod.linkLibrary(deps.core_lib);
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    // The converter transcodes real images, so the default build optimizes
    // like the C-era tool build did; an explicit -Doptimize still wins.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    const webp_dep = b.dependency("webp", .{ .target = target, .optimize = optimize });
    const jof_dep = b.dependency("jof", .{ .target = target, .optimize = optimize });
    const compress_dep = b.dependency("compress", .{ .target = target, .optimize = optimize });
    const reflow_dep = b.dependency("reflow", .{ .target = target, .optimize = optimize });
    const rabook_dep = b.dependency("rabook_compile", .{ .target = target, .optimize = optimize });
    const deps = Deps{
        .fmt_lib = b.dependency("ra8_fmt", .{ .target = target, .optimize = optimize }).artifact("ra8_fmt"),
        .webp_lib = webp_dep.artifact("webp"),
        .webp_vendor_lib = webp_dep.artifact("webp_vendor"),
        .imgdec_lib = b.dependency("ra8_imgdec", .{ .target = target, .optimize = optimize }).artifact("ra8_imgdec"),
        .mem_lib = b.dependency("ra8_mem", .{ .target = target, .optimize = optimize }).artifact("ra8_mem"),
        .jpeg_lib = b.dependency("ra8_jpeg", .{ .target = target, .optimize = optimize }).artifact("ra8_jpeg"),
        .core_lib = b.dependency("ra8_core", .{ .target = target, .optimize = optimize }).artifact("ra8_core_zig"),
        .jof_lib = jof_dep.artifact("jof"),
        .jof_c_tests = jof_dep.namedWriteFiles("c_tests").getDirectory(),
        .compress_lib = compress_dep.artifact("compress"),
        .miniz_lib = compress_dep.artifact("miniz"),
        .miniz_host_lib = compress_dep.artifact("miniz_host"),
        .book_lib = b.dependency("book", .{ .target = target, .optimize = optimize }).artifact("ra8_book"),
        .io_include_dir = b.dependency("ra8_io", .{ .target = target, .optimize = optimize }).artifact("ra8_io").getEmittedIncludeTree(),
        .reflow_include_dir = reflow_dep.artifact("ra8_reflow_parse").getEmittedIncludeTree(),
        .stb_image_lib = reflow_dep.artifact("stb_image"),
        .rabook_compile_lib = rabook_dep.artifact("rabook_compile"),
        .rabook_compile_fixture_lib = rabook_dep.artifact("rabook_compile_fixture"),
    };

    const tool_sources = [_][]const u8{
        "src/main.c",
        "src/ra8_fmt_host_fd.c",
        "src/ra8_fmt_host_spool.c",
        "src/ra8_fmt_portable_convert.c",
        "src/ra8_fmt_portable_verify.c",
        "src/ra8_fmt_portable_main.c",
    };

    // Production CLI binary enforces zero allocator/mmap symbols via
    // check_production_surface.sh, so disable C-UBSan on `mod` alone (avoiding
    // libubsan_rt's malloc/free/munmap in debug builds) while test_exes keep
    // default C sanitization.
    const mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .sanitize_c = .off,
    });
    mod.link_libc = true;
    linkEngineDeps(b, mod, deps, true, false, false, false);
    addC(b, mod, ".", &tool_sources, &first_party_flags);
    const exe = b.addExecutable(.{ .name = "rabook_imagepack", .root_module = mod });
    b.installArtifact(exe);

    // ---- unit targets (the C-era add_test set) ----
    const Unit = struct {
        name: []const u8,
        files: []const []const u8 = &.{},
        jof_c_file: ?[]const u8 = null,
        webp: bool,
        with_support_inc: bool = false,
        with_jof_internal: bool = false,
        extra_defs: []const []const u8 = &.{},
        no_malloc_miniz: bool = false,
        stb_image: bool = false,
        rabook: bool = false,
    };

    const support_inc = "../../tests/support/inc";
    const units = [_]Unit{
        .{
            .name = "test_jof_audit_portable",
            .jof_c_file = "test_jof_audit.c",
            .webp = true,
            .with_support_inc = true,
        },
        .{
            .name = "test_jof_portable",
            .jof_c_file = "test_jof.c",
            .webp = true,
            .with_support_inc = true,
            .no_malloc_miniz = true,
        },
        .{
            .name = "test_jof_edges_portable",
            .jof_c_file = "test_jof_edges.c",
            .webp = true,
            .with_support_inc = true,
            .no_malloc_miniz = true,
        },
        .{
            .name = "test_jof_png_hostile_portable",
            .jof_c_file = "test_jof_png_hostile.c",
            .webp = true,
            .with_support_inc = true,
            .with_jof_internal = true,
            .no_malloc_miniz = true,
        },
        .{
            .name = "test_jof_probe_dims_portable",
            .jof_c_file = "test_jof_probe_dims.c",
            .webp = true,
            .with_support_inc = true,
            .no_malloc_miniz = true,
        },
        .{
            .name = "test_jof_produce_portable",
            .jof_c_file = "test_jof_produce.c",
            .webp = true,
            .with_support_inc = true,
            .no_malloc_miniz = true,
            .stb_image = true,
        },
        .{
            .name = "test_jof_produce_guards_portable",
            .jof_c_file = "test_jof_produce_guards.c",
            .webp = true,
            .with_support_inc = true,
            .with_jof_internal = true,
            .no_malloc_miniz = true,
        },
        .{
            .name = "test_jof_produce_reject_portable",
            .jof_c_file = "test_jof_produce_reject.c",
            .webp = true,
            .with_support_inc = true,
            .no_malloc_miniz = true,
        },
        .{
            .name = "test_jof_produce_webp_portable",
            .jof_c_file = "test_jof_produce_webp.c",
            .webp = true,
            .with_support_inc = true,
            .no_malloc_miniz = true,
        },
        .{
            .name = "test_ra8_fmt_stream_inspect",
            .files = &.{"tests/src/test_ra8_fmt_stream_inspect.c"},
            .webp = true,
        },
        .{
            .name = "test_ra8_fmt_host_fd",
            .files = &.{ "tests/src/test_ra8_fmt_host_fd.c", "src/ra8_fmt_host_fd.c" },
            .webp = false,
        },
        .{
            .name = "test_ra8_fmt_stream_convert",
            .files = &.{ "tests/src/test_ra8_fmt_stream_convert.c", "src/ra8_fmt_host_fd.c" },
            .webp = true,
            .extra_defs = &.{"-DRA8_FMT_TEST_PNG=\"../../tests/fixtures/rabook_fixed_layout/OEBPS/images/page1.png\""},
        },
        .{
            .name = "test_ra8_fmt_stream_verify",
            .files = &.{"tests/src/test_ra8_fmt_stream_verify.c"},
            .webp = true,
            .extra_defs = &.{"-DRA8_FMT_TEST_PNG=\"../../tests/fixtures/rabook_fixed_layout/OEBPS/images/page1.png\""},
        },
        .{
            .name = "test_ra8_fmt_stream_rabook",
            .files = &.{"tests/src/test_ra8_fmt_stream_rabook.c"},
            .webp = true,
            .with_support_inc = true,
            .rabook = true,
        },
    };

    const test_step = b.step("test", "Run the rabook_imagepack unit tests");
    {
        const zig_test_mod = b.createModule(.{
            .root_source_file = b.path("tests/rabook_imagepack_test.zig"),
            .target = target,
            .optimize = optimize,
        });
        const zig_tests = b.addTest(.{ .root_module = zig_test_mod });
        const run_zig_tests = b.addRunArtifact(zig_tests);
        run_zig_tests.setCwd(b.path("."));
        ra8_build.configureHostTestRun(b, test_step, zig_tests, run_zig_tests);
        test_step.dependOn(&run_zig_tests.step);
    }

    var rabook_fixture_exe: ?*std.Build.Step.Compile = null;
    for (units) |unit| {
        const tmod = b.createModule(.{ .target = target, .optimize = optimize });
        tmod.link_libc = true;
        linkEngineDeps(b, tmod, deps, unit.webp, unit.no_malloc_miniz, unit.stb_image, unit.rabook);
        if (unit.with_support_inc) tmod.addIncludePath(b.path(support_inc));
        if (unit.with_jof_internal) tmod.addIncludePath(deps.jof_c_tests);

        var unit_flags: std.ArrayList([]const u8) = .empty;
        unit_flags.appendSlice(b.allocator, &first_party_flags) catch @panic("oom");
        unit_flags.appendSlice(b.allocator, unit.extra_defs) catch @panic("oom");
        if (unit.files.len > 0) addC(b, tmod, ".", unit.files, unit_flags.items);
        if (unit.jof_c_file) |c_file| {
            tmod.addCSourceFile(.{
                .file = deps.jof_c_tests.path(b, c_file),
                .flags = unit_flags.items,
            });
        }
        const test_exe = b.addExecutable(.{ .name = unit.name, .root_module = tmod });
        if (std.mem.eql(u8, unit.name, "test_ra8_fmt_stream_rabook")) {
            rabook_fixture_exe = test_exe;
        }
        b.installArtifact(test_exe);
        const run_test = b.addRunArtifact(test_exe);
        run_test.setCwd(b.path("."));
        test_step.dependOn(&run_test.step);
    }

    // ---- the four C-era integration scripts, driven against the built exes ----
    const integration_step = b.step("integration", "Run the rabook_imagepack integration suites");
    {
        const cmd = b.addSystemCommand(&.{ "sh", "tests/rabook_inspect_integration.sh" });
        cmd.setCwd(b.path("."));
        cmd.addArtifactArg(exe);
        cmd.addArtifactArg(rabook_fixture_exe.?);
        integration_step.dependOn(&cmd.step);
    }
    {
        const cmd = b.addSystemCommand(&.{ "sh", "tests/convert_integration.sh" });
        cmd.setCwd(b.path("."));
        cmd.addArtifactArg(exe);
        cmd.addArg("../..");
        integration_step.dependOn(&cmd.step);
    }
    {
        const cmd = b.addSystemCommand(&.{ "sh", "tests/verify_integration.sh" });
        cmd.setCwd(b.path("."));
        cmd.addArtifactArg(exe);
        cmd.addArg("../..");
        integration_step.dependOn(&cmd.step);
    }
    {
        const cmd = b.addSystemCommand(&.{ "sh", "tests/check_production_surface.sh" });
        cmd.setCwd(b.path("."));
        cmd.addArg(".");
        cmd.addArtifactArg(exe);
        integration_step.dependOn(&cmd.step);
    }
}
