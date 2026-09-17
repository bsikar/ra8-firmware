// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
// Exit-status contract of `check_since_version` (#858), pinned against a
// temporary tree so every status the deleted Python could produce is proved
// here: 0 clean, 1 a problem or a bad VERSION, 2 a usage error.

const std = @import("std");
const cli = @import("cli");

const testing = std.testing;

const Streams = struct {
    out: std.ArrayList(u8),
    err: std.ArrayList(u8),

    fn init() Streams {
        return .{
            .out = std.ArrayList(u8).init(testing.allocator),
            .err = std.ArrayList(u8).init(testing.allocator),
        };
    }

    fn deinit(self: *Streams) void {
        self.out.deinit();
        self.err.deinit();
    }
};

/// One temporary repository, plus the arena every run allocates from.
const Fixture = struct {
    tmp: std.testing.TmpDir,
    arena: std.heap.ArenaAllocator,

    fn init() Fixture {
        return .{
            .tmp = std.testing.tmpDir(.{ .iterate = true }),
            .arena = std.heap.ArenaAllocator.init(testing.allocator),
        };
    }

    fn deinit(self: *Fixture) void {
        self.arena.deinit();
        self.tmp.cleanup();
    }

    fn write(self: *Fixture, path: []const u8, bytes: []const u8) !void {
        if (std.fs.path.dirname(path)) |parent| {
            try self.tmp.dir.makePath(parent);
        }
        try self.tmp.dir.writeFile(.{ .sub_path = path, .data = bytes });
    }

    fn run(self: *Fixture, argv: []const []const u8, streams: *Streams) !u8 {
        return cli.run(
            self.arena.allocator(),
            self.tmp.dir,
            argv,
            ".",
            streams.out.writer(),
            streams.err.writer(),
        );
    }
};

test "no arguments is a usage error" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{"check_since_version"}, &streams);
    try testing.expectEqual(@as(u8, 2), status);
    try testing.expectEqualStrings(cli.usage ++ "\n", streams.err.items);
}

test "the usage line names both accepted forms" {
    try testing.expectEqualStrings(
        "usage: check_since_version FILE [FILE ...] | --all",
        cli.usage,
    );
}

test "a clean file exits zero and prints nothing" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write("a.c", "/** @since 0.1.0 */\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "a.c" }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings("", streams.out.items);
    try testing.expectEqualStrings("", streams.err.items);
}

test "a wrong value exits one and reports on stderr" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write("a.c", "/** @since 9.9.9 */\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "a.c" }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expectEqualStrings("", streams.out.items);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.err.items,
        "check_since_version: project version is 0.1.0",
    ) != null);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.err.items,
        "@since 9.9.9 != project 0.1.0",
    ) != null);
    try testing.expect(std.mem.endsWith(u8, streams.err.items, "\n1 issue(s) found.\n"));
}

test "the reported path is the resolved absolute path" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write("a.c", "/** @since 9.9.9 */\n");
    var streams = Streams.init();
    defer streams.deinit();

    _ = try fixture.run(&[_][]const u8{ "check_since_version", "a.c" }, &streams);
    try testing.expect(std.mem.indexOf(u8, streams.err.items, "/a.c:1:") != null);
}

test "several problems are counted together" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write("a.c", "@since 1.0.0\n@since 2.0.0\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "a.c" }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.endsWith(u8, streams.err.items, "\n2 issue(s) found.\n"));
}

test "a missing file is skipped, not an error" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "absent.c" }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings("", streams.err.items);
}

test "a directory argument is skipped, as is_file() skipped it" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write("sub/a.c", "@since 9.9.9\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "sub" }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
}

test "a non-source suffix has no value check" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write("notes.md", "@since 9.9.9\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "notes.md" }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
}

test "a public header with an untagged declaration fails the presence half" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write("libs/ra8_gpio/inc/ra8_gpio.h", "ra8_err_t ra8_gpio_init(void);\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(
        &[_][]const u8{ "check_since_version", "libs/ra8_gpio/inc/ra8_gpio.h" },
        &streams,
    );
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.err.items,
        "ra8_gpio_init missing @since",
    ) != null);
}

test "a private source is not held to the presence half" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write("libs/ra8_gpio/src/ra8_gpio.c", "ra8_err_t ra8_gpio_init(void);\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(
        &[_][]const u8{ "check_since_version", "libs/ra8_gpio/src/ra8_gpio.c" },
        &streams,
    );
    try testing.expectEqual(@as(u8, 0), status);
}

test "a tagged public header passes both halves" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write(
        "libs/ra8_gpio/inc/ra8_gpio.h",
        "/** @since 0.1.0 */\nra8_err_t ra8_gpio_init(void);\n",
    );
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(
        &[_][]const u8{ "check_since_version", "libs/ra8_gpio/inc/ra8_gpio.h" },
        &streams,
    );
    try testing.expectEqual(@as(u8, 0), status);
}

test "a public header can fail both halves at once" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write(
        "libs/ra8_gpio/inc/ra8_gpio.h",
        "ra8_err_t ra8_gpio_init(void);\n/** @since 9.9.9 */\n",
    );
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(
        &[_][]const u8{ "check_since_version", "libs/ra8_gpio/inc/ra8_gpio.h" },
        &streams,
    );
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.endsWith(u8, streams.err.items, "\n2 issue(s) found.\n"));
}

test "several files are all checked" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write("a.c", "@since 9.9.9\n");
    try fixture.write("b.c", "@since 0.1.0\n");
    try fixture.write("c.c", "@since 8.8.8\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(
        &[_][]const u8{ "check_since_version", "a.c", "b.c", "c.c" },
        &streams,
    );
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.endsWith(u8, streams.err.items, "\n2 issue(s) found.\n"));
}

test "a missing VERSION file exits one with the create-it message" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("a.c", "@since 0.1.0\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "a.c" }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.err.items,
        "missing -- create it with a single semver line",
    ) != null);
}

test "a non-semver VERSION file exits one and quotes the content" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "1.2\n");
    try fixture.write("a.c", "@since 1.2\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "a.c" }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.err.items,
        "content '1.2' is not semver MAJOR.MINOR.PATCH",
    ) != null);
}

test "the VERSION file is stripped before it is parsed" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "  0.1.0  \n\n");
    try fixture.write("a.c", "@since 0.1.0\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "a.c" }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
}

test "VERSION is read before any file is scanned" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "nope\n");
    try fixture.write("a.c", "@since 9.9.9\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "a.c" }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(u8, streams.err.items, "issue(s) found") == null);
}

test "an unreadable repository root is a usage error" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    var streams = Streams.init();
    defer streams.deinit();

    const status = try cli.run(
        fixture.arena.allocator(),
        fixture.tmp.dir,
        &[_][]const u8{ "check_since_version", "a.c" },
        "no/such/root",
        streams.out.writer(),
        streams.err.writer(),
    );
    try testing.expectEqual(@as(u8, 2), status);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.err.items,
        "cannot open repository root",
    ) != null);
}

test "--repo-root overrides the environment value" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("inner/VERSION", "2.0.0\n");
    try fixture.write("inner/a.c", "@since 2.0.0\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try cli.run(
        fixture.arena.allocator(),
        fixture.tmp.dir,
        &[_][]const u8{ "check_since_version", "--repo-root", "inner", "inner/a.c" },
        ".",
        streams.out.writer(),
        streams.err.writer(),
    );
    try testing.expectEqual(@as(u8, 0), status);
}

test "--repo-root= takes its value inline" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("inner/VERSION", "2.0.0\n");
    try fixture.write("inner/a.c", "@since 9.9.9\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try cli.run(
        fixture.arena.allocator(),
        fixture.tmp.dir,
        &[_][]const u8{ "check_since_version", "--repo-root=inner", "inner/a.c" },
        ".",
        streams.out.writer(),
        streams.err.writer(),
    );
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.err.items,
        "project version is 2.0.0",
    ) != null);
}

test "--repo-root with no value is a usage error" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(
        &[_][]const u8{ "check_since_version", "--repo-root" },
        &streams,
    );
    try testing.expectEqual(@as(u8, 2), status);
    try testing.expectEqualStrings(cli.usage ++ "\n", streams.err.items);
}

test "--all below the tracked floor exits two rather than reading clean" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "--all" }, &streams);
    try testing.expectEqual(@as(u8, 2), status);
    try testing.expect(std.mem.indexOf(u8, streams.err.items, "FATAL") != null);
}

test "--selftest on a collapsed enumeration exits two" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "--selftest" }, &streams);
    try testing.expectEqual(@as(u8, 2), status);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.out.items,
        "  [ok] a wrong @since value fires",
    ) != null);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.out.items,
        "  [ok] the correct @since value stays quiet",
    ) != null);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.out.items,
        "  [ok] a public decl missing @since fires",
    ) != null);
}

test "--selftest names itself on the first line" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    var streams = Streams.init();
    defer streams.deinit();

    _ = try fixture.run(&[_][]const u8{ "check_since_version", "--selftest" }, &streams);
    try testing.expect(std.mem.startsWith(
        u8,
        streams.out.items,
        "check_since_version --selftest\n",
    ));
}

test "--selftest with a bad VERSION exits one before any assertion" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "nope\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "--selftest" }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(u8, streams.out.items, "[ok]") == null);
}

test "--selftest is honoured anywhere in argv" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "nope\n");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(
        &[_][]const u8{ "check_since_version", "a.c", "--selftest" },
        &streams,
    );
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(u8, streams.err.items, "not semver") != null);
}

test "a file whose bytes are not UTF-8 is skipped" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    try fixture.write("a.c", "@since 9.9.9\n\xFF\xFE");
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "a.c" }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
}

test "the exit statuses are the three the Python had" {
    try testing.expectEqual(@as(u8, 0), cli.exit_ok);
    try testing.expectEqual(@as(u8, 1), cli.exit_problems);
    try testing.expectEqual(@as(u8, 2), cli.exit_usage);
}

test "a source far past any read ceiling still reports its wrong @since" {
    var fixture = Fixture.init();
    defer fixture.deinit();
    try fixture.write("VERSION", "0.1.0\n");
    {
        // Written sparse: the tag sits on line 1, the hole carries the size
        // past the 16 MiB ceiling this read used to carry, and the file
        // costs neither disk nor runtime.
        var file = try fixture.tmp.dir.createFile("big.c", .{});
        defer file.close();
        try file.writeAll("/** @since 9.9.9 */\n");
        try file.seekTo(17 * 1024 * 1024);
        try file.writeAll("\n");
    }
    var streams = Streams.init();
    defer streams.deinit();

    const status = try fixture.run(&[_][]const u8{ "check_since_version", "big.c" }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.err.items,
        ":1: @since 9.9.9 != project 0.1.0",
    ) != null);
}
