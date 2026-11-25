//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig book archive (RA8FW-1242, RA8FW-1244,
//! RA8FW-1248, RA8FW-1249, RA8FW-1250):
//! src/book_root.zig aggregates the ported TUs (book_stream_wire.zig, then
//! book_chunked.zig) so their C ABI exports land in the installed static
//! library. CMake consumes it through those symbol names; the remaining
//! book C TUs call them unchanged.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    _ = b.addModule("ra8_book", .{
        .root_source_file = b.path("src/book_root.zig"),
    });

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/book_root.zig"),
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
    // Split functions and data so --gc-sections discards unused book_paged externs on ARM.
    library.link_function_sections = true;
    library.link_data_sections = true;
    library.root_module.pic = true;
    library.installHeadersDirectory(b.path("inc"), "", .{});
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig book archive tests");
    const suites = [_]struct { alias: []const u8, source: []const u8, root: []const u8 }{
        .{ .alias = "wire", .source = "src/book_stream_wire.zig", .root = "tests/book_stream_wire_test.zig" },
        .{ .alias = "chunked", .source = "src/book_chunked.zig", .root = "tests/book_chunked_test.zig" },
        .{ .alias = "validate", .source = "src/book_chunked_validate.zig", .root = "tests/book_chunked_validate_test.zig" },
        .{ .alias = "stream", .source = "src/book_stream.zig", .root = "tests/book_stream_test.zig" },
        .{ .alias = "paged", .source = "src/book_paged.zig", .root = "tests/book_paged_test.zig" },
        .{ .alias = "xhtml", .source = "src/book_xhtml.zig", .root = "tests/book_xhtml_test.zig" },
        .{ .alias = "book", .source = "src/book.zig", .root = "tests/book_test.zig" },
    };
    for (suites) |suite| {
        const sut = b.createModule(.{
            .root_source_file = b.path(suite.source),
            .target = target,
            .optimize = optimize,
        });
        const mod = b.createModule(.{
            .root_source_file = b.path(suite.root),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        mod.addImport(suite.alias, sut);
        const test_exe = b.addTest(.{ .root_module = mod });
        test_step.dependOn(&b.addRunArtifact(test_exe).step);
    }
}
