//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig side of `ra8_mem`: the slab allocator, the page
//! cache, the byte-stream adapter over it, the glyph cache, the tile cache,
//! the object-source registry and the init-time bump arena. CMake consumes
//! the installed static library
//! through the unchanged `inc/ra8_arena.h`, `inc/ra8_slab.h`, `inc/ra8_vmem.h`,
//! `inc/ra8_vmem_stream.h`, `inc/ra8_glyph_atlas.h`, `inc/ra8_tile_cache.h`
//! and `inc/ra8_vsource.h`.
//!
//! The page cache, the glyph atlas and the tile cache are typed facades over
//! the Zig `keycache` in this same archive, so no `ra8_keycache_*` symbol is
//! left undefined and `ra8_vmem_get`/`ra8_vmem_put` resolve in-archive too.
//! The source registry adds no extern of its own: a paged object's read
//! callback is a pointer it is handed, not a link-time symbol.
//!
//! `libs/ra8_mem/src` has no C left. The arena was the last translation unit
//! in it, and its seven `ra8_arena_*` symbols now come out of this archive
//! through the unchanged `inc/ra8_arena.h` (#2601).

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_mem_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });

    const library = b.addLibrary(.{
        .name = "ra8_mem",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_mem tests");

    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "arena", .source = "src/internal/arena.zig", .root = "tests/arena_test.zig" },
        .{ .name = "vocab", .source = "src/internal/vocab.zig", .root = "tests/vocab_test.zig" },
        .{ .name = "slab", .source = "src/internal/slab.zig", .root = "tests/slab_test.zig" },
        .{
            .name = "vmem_stream",
            .source = "src/internal/vmem_stream.zig",
            .root = "tests/vmem_stream_test.zig",
        },
        .{
            .name = "glyph_atlas",
            .source = "src/internal/glyph_atlas.zig",
            .root = "tests/glyph_atlas_test.zig",
        },
        .{
            .name = "vsource",
            .source = "src/internal/vsource.zig",
            .root = "tests/vsource_test.zig",
        },
        .{ .name = "vmem", .source = "src/internal/vmem.zig", .root = "tests/vmem_test.zig" },
        .{
            .name = "tile_geometry",
            .source = "src/internal/tile_geometry.zig",
            .root = "tests/tile_geometry_test.zig",
        },
        .{
            .name = "tile_cache",
            .source = "src/internal/tile_cache.zig",
            .root = "tests/tile_cache_test.zig",
        },
        .{
            .name = "keycache_list",
            .source = "src/internal/keycache_list.zig",
            .root = "tests/keycache_list_test.zig",
        },
        .{
            .name = "keycache_index",
            .source = "src/internal/keycache_index.zig",
            .root = "tests/keycache_index_test.zig",
        },
        .{
            .name = "keycache_policy",
            .source = "src/internal/keycache_policy.zig",
            .root = "tests/keycache_policy_test.zig",
        },
        .{
            .name = "keycache",
            .source = "src/internal/keycache.zig",
            .root = "tests/keycache_test.zig",
        },
    };
    for (units) |unit| {
        const under_test = b.createModule(.{
            .root_source_file = b.path(unit.source),
            .target = target,
            .optimize = optimize,
        });
        const test_module = b.createModule(.{
            .root_source_file = b.path(unit.root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport(unit.name, under_test);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
