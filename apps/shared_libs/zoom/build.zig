//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the zoom facade
//! (`apps/shared_libs/zoom`). CMake consumes the installed static library
//! through the unchanged `inc/zoom.h` / `src/zoom_internal.h` C ABI; the
//! `test` step covers the viewport state machine (`src/zoom.zig`), the strip
//! composite (`src/zoom_render.zig`), the `.rabook` adapter
//! (`src/zoom_book.zig`), the tiled-atlas adapter (`src/zoom_tiles.zig`) and
//! the ABI membrane (`src/zoom_abi.zig`) driven
//! over the C-shaped structs.
//!
//! Template: libs/ra8_ftl/build.zig (module + static library + host test
//! step shape). No build options: the viewport engine is configured
//! entirely through `zoom_view_cfg_t` at open time, so nothing here is
//! configured at compile time.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/zoom_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "zoom",
        .linkage = .static,
        .root_module = library_module,
    });
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    // Split functions and data so --gc-sections can discard unused Zig code
    // from the single-object archive in firmware images.
    library.link_function_sections = true;
    library.link_data_sections = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    // Standalone host suites mirror the production sources under tests/ so
    // each test binary pulls only the mocks its adapter expects.
    const zoom_mod = b.createModule(.{
        .root_source_file = b.path("src/zoom.zig"),
        .target = target,
        .optimize = optimize,
    });
    const book_mod = b.createModule(.{
        .root_source_file = b.path("src/zoom_book.zig"),
        .target = target,
        .optimize = optimize,
    });
    const render_mod = b.createModule(.{
        .root_source_file = b.path("src/zoom_render.zig"),
        .target = target,
        .optimize = optimize,
    });
    const tiles_mod = b.createModule(.{
        .root_source_file = b.path("src/zoom_tiles.zig"),
        .target = target,
        .optimize = optimize,
    });
    const mem_mod = b.createModule(.{
        .root_source_file = b.path("src/zoom_mem.zig"),
        .target = target,
        .optimize = optimize,
    });
    const popup_mod = b.createModule(.{
        .root_source_file = b.path("src/zoom_popup.zig"),
        .target = target,
        .optimize = optimize,
    });

    const zoom_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/zoom_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    zoom_test_mod.addImport("zoom", zoom_mod);
    const implementation_tests = b.addTest(.{ .root_module = zoom_test_mod });

    const abi_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/zoom_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_test_mod.addImport("abi", library_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_mod });

    const book_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/zoom_book_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    book_test_mod.addImport("book", book_mod);
    const book_tests = b.addTest(.{ .root_module = book_test_mod });

    const render_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/zoom_render_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    render_test_mod.addImport("render", render_mod);
    const render_tests = b.addTest(.{ .root_module = render_test_mod });

    const tiles_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/zoom_tiles_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    tiles_test_mod.addImport("tiles", tiles_mod);
    const tiles_tests = b.addTest(.{ .root_module = tiles_test_mod });

    const mem_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/zoom_mem_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    mem_test_mod.addImport("mem", mem_mod);
    const mem_tests = b.addTest(.{ .root_module = mem_test_mod });

    const popup_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/zoom_popup_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    popup_test_mod.addImport("popup", popup_mod);
    const popup_tests = b.addTest(.{ .root_module = popup_test_mod });

    const run_implementation_tests = b.addRunArtifact(implementation_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig zoom tests");
    test_step.dependOn(&run_implementation_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&b.addRunArtifact(book_tests).step);
    test_step.dependOn(&b.addRunArtifact(render_tests).step);
    test_step.dependOn(&b.addRunArtifact(tiles_tests).step);
    test_step.dependOn(&b.addRunArtifact(mem_tests).step);
    test_step.dependOn(&b.addRunArtifact(popup_tests).step);
}
