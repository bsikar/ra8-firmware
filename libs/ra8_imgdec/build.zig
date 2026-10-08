//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_imgdec`. CMake consumes the
//! installed static library through the unchanged headers in `inc/`.
//!
//! The archive leaves two symbols unresolved, `ra8_arena_carve` and
//! `ra8_arena_remaining`, which the ring below still provides in C. Both are
//! named only in `src/ra8_imgdec_abi.zig`; everything under `src/internal`
//! reaches the arena through an injected seam, so the whole of the decision
//! logic is covered by the host suite below.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_imgdec_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_imgdec",
        .linkage = .static,
        .root_module = library_module,
    });
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_imgdec tests");

    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "sniff", .source = "src/internal/sniff.zig", .root = "tests/sniff_test.zig" },
        .{ .name = "dims", .source = "src/internal/dims.zig", .root = "tests/dims_test.zig" },
        .{ .name = "name", .source = "src/internal/name.zig", .root = "tests/name_test.zig" },
        .{ .name = "scratch", .source = "src/internal/scratch.zig", .root = "tests/scratch_test.zig" },
        .{ .name = "fabric", .source = "src/internal/fabric.zig", .root = "tests/fabric_test.zig" },
        .{ .name = "mux", .source = "src/internal/mux.zig", .root = "tests/mux_test.zig" },
        .{ .name = "vocab", .source = "src/internal/abi.zig", .root = "tests/abi_test.zig" },
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
