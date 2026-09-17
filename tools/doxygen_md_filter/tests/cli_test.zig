//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract of `doxygen_md_filter` (#858). Doxygen reads the
//! filtered page from stdout and treats a non-zero status as a failed input
//! filter, so these cases pin 0 / 1 / 2, which stream each message lands on,
//! and that nothing but the page itself reaches stdout.

const std = @import("std");
const cli = @import("cli");

const Streams = struct {
    out: std.ArrayList(u8),
    err: std.ArrayList(u8),

    fn init(allocator: std.mem.Allocator) Streams {
        return .{
            .out = std.ArrayList(u8).init(allocator),
            .err = std.ArrayList(u8).init(allocator),
        };
    }

    fn deinit(self: *Streams) void {
        self.out.deinit();
        self.err.deinit();
    }
};

fn run(
    dir: std.fs.Dir,
    streams: *Streams,
    argv: []const []const u8,
    environment_root: ?[]const u8,
) !u8 {
    return cli.run(
        std.testing.allocator,
        dir,
        argv,
        environment_root,
        streams.out.writer(),
        streams.err.writer(),
    );
}

/// A small repository: a page, a README beside it, a nested README.
fn writeRepository(dir: std.fs.Dir) !void {
    try dir.makePath("docs");
    try dir.makePath("libs/ra8_fonts");
    try dir.writeFile(.{ .sub_path = "docs/README.md", .data = "# docs\n" });
    try dir.writeFile(.{ .sub_path = "libs/ra8_fonts/README.md", .data = "# fonts\n" });
}

test "exit 2 and usage on stderr when no file is named" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &.{"doxygen_md_filter"}, null);

    try std.testing.expectEqual(cli.exit_usage, status);
    try std.testing.expectEqualStrings(cli.usage ++ "\n", streams.err.items);
    try std.testing.expectEqualStrings("", streams.out.items);
}

test "exit 2 when a second file is named" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &.{ "doxygen_md_filter", "a.md", "b.md" }, null);

    try std.testing.expectEqual(cli.exit_usage, status);
    try std.testing.expectEqualStrings("", streams.out.items);
}

test "exit 2 on an unknown flag" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &.{ "doxygen_md_filter", "--strip", "a.md" }, null);

    try std.testing.expectEqual(cli.exit_usage, status);
}

test "exit 2 when --repo-root has no value" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &.{ "doxygen_md_filter", "--repo-root" }, null);

    try std.testing.expectEqual(cli.exit_usage, status);
}

test "exit 1 when the page cannot be read" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &.{ "doxygen_md_filter", "missing.md" }, null);

    try std.testing.expectEqual(cli.exit_error, status);
    try std.testing.expectEqualStrings("", streams.out.items);
    try std.testing.expect(std.mem.indexOf(u8, streams.err.items, "missing.md") != null);
}

test "exit 1 when the repository root cannot be opened" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();
    try tmp.dir.writeFile(.{ .sub_path = "page.md", .data = "text\n" });

    const status = try run(
        tmp.dir,
        &streams,
        &.{ "doxygen_md_filter", "--repo-root", "nowhere", "page.md" },
        null,
    );

    try std.testing.expectEqual(cli.exit_error, status);
    try std.testing.expectEqualStrings("", streams.out.items);
}

test "exit 0 and the filtered page on stdout" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();
    try writeRepository(tmp.dir);
    try tmp.dir.writeFile(.{
        .sub_path = "page.md",
        .data = "[![CI](https://github.com/o/r/actions/workflows/ci.yml/badge.svg)](https://github.com/o/r)\n" ++
            "see [fonts](libs/ra8_fonts/README.md)\n",
    });

    const status = try run(tmp.dir, &streams, &.{ "doxygen_md_filter", "page.md" }, null);

    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqualStrings("see [fonts](@ref libs/ra8_fonts)\n", streams.out.items);
    try std.testing.expectEqualStrings("", streams.err.items);
}

test "a page in a subdirectory resolves links from its own directory" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();
    try writeRepository(tmp.dir);
    try tmp.dir.writeFile(.{
        .sub_path = "docs/page.md",
        .data = "see [fonts](../libs/ra8_fonts/README.md)\n",
    });

    const status = try run(tmp.dir, &streams, &.{ "doxygen_md_filter", "docs/page.md" }, null);

    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqualStrings("see [fonts](@ref libs/ra8_fonts)\n", streams.out.items);
}

test "the environment names the repository root when no flag does" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();
    try tmp.dir.makePath("repo");
    var repo = try tmp.dir.openDir("repo", .{});
    defer repo.close();
    try writeRepository(repo);
    try repo.writeFile(.{ .sub_path = "page.md", .data = "see [docs](docs/README.md)\n" });

    const status = try run(tmp.dir, &streams, &.{ "doxygen_md_filter", "repo/page.md" }, "repo");

    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqualStrings("see [docs](@ref docs)\n", streams.out.items);
}

test "an explicit --repo-root outranks the environment" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();
    try tmp.dir.makePath("repo");
    var repo = try tmp.dir.openDir("repo", .{});
    defer repo.close();
    try writeRepository(repo);
    try repo.writeFile(.{ .sub_path = "page.md", .data = "see [docs](docs/README.md)\n" });

    const status = try run(
        tmp.dir,
        &streams,
        &.{ "doxygen_md_filter", "--repo-root", "repo", "repo/page.md" },
        "nowhere",
    );

    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqualStrings("see [docs](@ref docs)\n", streams.out.items);
}

test "a page outside the repository root keeps badge stripping" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();
    try tmp.dir.makePath("repo");
    var repo = try tmp.dir.openDir("repo", .{});
    defer repo.close();
    try writeRepository(repo);
    try tmp.dir.writeFile(.{
        .sub_path = "outside.md",
        .data = "![CI](https://github.com/o/r/actions/workflows/ci.yml/badge.svg)\nkeep\n",
    });

    const status = try run(
        tmp.dir,
        &streams,
        &.{ "doxygen_md_filter", "--repo-root", "repo", "outside.md" },
        null,
    );

    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqualStrings("keep\n", streams.out.items);
}

test "a page with nothing to filter is reproduced byte for byte" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();
    try writeRepository(tmp.dir);
    const page = "# Title\n\nBody with [a link](https://example.invalid).\n\n```\ncode\n```\n";
    try tmp.dir.writeFile(.{ .sub_path = "page.md", .data = page });

    const status = try run(tmp.dir, &streams, &.{ "doxygen_md_filter", "page.md" }, null);

    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqualStrings(page, streams.out.items);
}

test "a CRLF page is emitted with LF terminators" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();
    try writeRepository(tmp.dir);
    try tmp.dir.writeFile(.{
        .sub_path = "page.md",
        .data = "# Title\r\n\r\nsee [docs](docs/README.md)\r\n",
    });

    const status = try run(tmp.dir, &streams, &.{ "doxygen_md_filter", "page.md" }, null);

    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqualStrings("# Title\n\nsee [docs](@ref docs)\n", streams.out.items);
}

test "a page holding invalid UTF-8 is filtered as bytes, not rejected" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();
    try writeRepository(tmp.dir);
    try tmp.dir.writeFile(.{
        .sub_path = "page.md",
        .data = "\xff\xfe stray\nsee [docs](docs/README.md)\n",
    });

    const status = try run(tmp.dir, &streams, &.{ "doxygen_md_filter", "page.md" }, null);

    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqualStrings(
        "\xff\xfe stray\nsee [docs](@ref docs)\n",
        streams.out.items,
    );
}
