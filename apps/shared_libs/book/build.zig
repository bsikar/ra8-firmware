//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig book wire port (RA8FW-1242): src/book_stream_wire.zig
//! owns the five `priv_book_stream_*` seams behind the unchanged declarations
//! in src/book_stream_internal.h. CMake consumes the installed static library
//! through those symbol names; the remaining book C TUs call them unchanged.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/book_stream_wire.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_book",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig book wire tests");
    const test_module = b.createModule(.{
        .root_source_file = b.path("src/book_stream_wire_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    const book_wire_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(book_wire_test).step);
}
