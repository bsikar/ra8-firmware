//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status and enumeration tests for the example board-pin gate (#858).
//! These drive `cli.run` over a real temporary tree, so the contract
//! `scripts/builders/check_example_board_pins.sh` passes through is pinned
//! end to end: the argv branch, the whole-tree sweep, the floor collapse, the
//! selftest and which stream each message lands on.

const std = @import("std");
const testing = std.testing;
const cli = @import("cli");

const idiom = "  cfg.pin = ((uint16_t)k_ra8_port_6 << 8) | (uint16_t)k_ra8_pin_11;\n";
const clean = "  cfg.pin = ra8_board_sw_pin(k_ra8_board_sw_user);\n";

const Harness = struct {
    tmp: std.testing.TmpDir,
    arena: std.heap.ArenaAllocator,
    stdout: std.ArrayList(u8),
    stderr: std.ArrayList(u8),
    root: []const u8,

    fn init() !Harness {
        var tmp = std.testing.tmpDir(.{});
        var arena = std.heap.ArenaAllocator.init(testing.allocator);
        const root = try tmp.dir.realpathAlloc(arena.allocator(), ".");
        return .{
            .tmp = tmp,
            .arena = arena,
            .stdout = std.ArrayList(u8).init(testing.allocator),
            .stderr = std.ArrayList(u8).init(testing.allocator),
            .root = root,
        };
    }

    fn deinit(self: *Harness) void {
        self.stdout.deinit();
        self.stderr.deinit();
        self.arena.deinit();
        self.tmp.cleanup();
    }

    fn write(self: *Harness, path: []const u8, body: []const u8) !void {
        if (std.fs.path.dirname(path)) |parent| try self.tmp.dir.makePath(parent);
        try self.tmp.dir.writeFile(.{ .sub_path = path, .data = body });
    }

    /// `count` example sources, so a sweep can clear the floor.
    fn populate(self: *Harness, count: usize, body: []const u8) !void {
        var index: usize = 0;
        while (index < count) : (index += 1) {
            const path = try std.fmt.allocPrint(
                self.arena.allocator(),
                "examples/app{d}/src/main.c",
                .{index},
            );
            try self.write(path, body);
        }
    }

    fn run(self: *Harness, argv: []const []const u8) !u8 {
        return cli.run(
            self.arena.allocator(),
            self.tmp.dir,
            self.root,
            argv,
            self.stdout.writer(),
            self.stderr.writer(),
        );
    }
};

test "a clean sweep over enough files exits 0 and reports the count on stdout" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stdout.items, "none hand-encode a board pin") != null);
    try testing.expectEqualStrings("", harness.stderr.items);
}

test "one hand-encoded pin in the sweep exits 1 and reports on stderr" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    try harness.write("examples/bad/src/main.c", idiom);
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "1 hand-encoded board pin(s)") != null);
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "examples/bad/src/main.c:1") != null);
    try testing.expectEqualStrings("", harness.stdout.items);
}

test "the findings carry the guidance tail" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    try harness.write("examples/bad/src/main.c", idiom);
    _ = try harness.run(&[_][]const u8{});
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "libs/ra8_board_ek_ra8d2") != null);
}

test "a sweep below the floor exits 2 rather than reporting a clean tree" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(10, clean);
    try testing.expectEqual(@as(u8, 2), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "FATAL") != null);
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "floor is 320") != null);
}

test "an empty examples tree exits 2, not 0" {
    var harness = try Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 2), try harness.run(&[_][]const u8{}));
}

test "the floor applies to the sweep only, so an argv list that filters to nothing exits 0" {
    var harness = try Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"README.md"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "no files to scan") != null);
}

test "an argv file with the idiom exits 1 with no floor in the way" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/bad/src/main.c", idiom);
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"examples/bad/src/main.c"}));
}

test "an argv file that is clean exits 0 and counts itself as scanned" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/ok/src/main.c", clean);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"examples/ok/src/main.c"}));
    try testing.expect(std.mem.indexOf(u8, harness.stdout.items, "1 example file(s) scanned") != null);
}

test "an argv directory is swept recursively" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/tree/a/main.c", clean);
    try harness.write("examples/tree/b/deep/other.cpp", idiom);
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"examples/tree"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "examples/tree/b/deep/other.cpp:1") != null);
}

test "an in-source build tree under examples is excluded from an argv list (#549)" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/x/build/gen.c", idiom);
    try harness.write("examples/x/src/main.c", clean);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{
        "examples/x/build/gen.c",
        "examples/x/src/main.c",
    }));
    try testing.expect(std.mem.indexOf(u8, harness.stdout.items, "1 example file(s) scanned") != null);
}

test "an in-source build tree is excluded from the sweep too" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    try harness.write("examples/x/build/gen.c", idiom);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
}

test "a tool-owned directory is excluded from the sweep" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    try harness.write("examples/x/__pycache__/gen.c", idiom);
    try harness.write("examples/x/.zig-cache/gen.h", idiom);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
}

test "a non-source suffix in argv is dropped rather than scanned" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/x/notes.txt", idiom);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"examples/x/notes.txt"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "no files to scan") != null);
}

test "an argv path that does not exist still counts as scanned and reads as nothing" {
    var harness = try Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"examples/gone/main.c"}));
    try testing.expect(std.mem.indexOf(u8, harness.stdout.items, "1 example file(s) scanned") != null);
}

test "an unknown flag is treated as a path, so there is no usage status" {
    var harness = try Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"--nonsense"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "no files to scan") != null);
}

test "a directory named like a source file is listed, unreadable, and skipped" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    try harness.tmp.dir.makePath("examples/odd/weird.c");
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
    // It counted toward the sweep even though nothing could be read from it.
    try testing.expect(std.mem.indexOf(u8, harness.stdout.items, "321 example file(s) scanned") != null);
}

test "a dot-prefixed source file is swept, because pathlib's glob hides nothing" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    try harness.write("examples/x/.hidden.c", idiom);
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
}

test "a file named exactly .c is swept but is not source on the argv path" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/x/.c", idiom);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"examples/x/.c"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "no files to scan") != null);
}

test "findings from several files are all reported" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/a/main.c", idiom);
    try harness.write("examples/b/main.c", idiom ++ idiom);
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{ "examples/a", "examples/b" }));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "3 hand-encoded board pin(s)") != null);
}

test "the same argv path twice is scanned twice, as the predecessor counted it" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/x/main.c", clean);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{
        "examples/x/main.c",
        "examples/x/main.c",
    }));
    try testing.expect(std.mem.indexOf(u8, harness.stdout.items, "2 example file(s) scanned") != null);
}

test "a ./ prefix on an argv path resolves the same way" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/x/main.c", idiom);
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"./examples/x/main.c"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "examples/x/main.c:1") != null);
}

test "an absolute argv path inside the tree reports repo-relative" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/x/main.c", idiom);
    const absolute = try std.fmt.allocPrint(
        harness.arena.allocator(),
        "{s}/examples/x/main.c",
        .{harness.root},
    );
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{absolute}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "examples/x/main.c:1") != null);
}

test "the four suffixes are all scanned" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/x/a.h", idiom);
    try harness.write("examples/x/b.cpp", idiom);
    try harness.write("examples/x/c.hpp", idiom);
    try harness.write("examples/x/d.c", idiom);
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"examples/x"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "4 hand-encoded board pin(s)") != null);
}

test "the sweep enumerates .c before .h, as the suffix loop did" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("examples/x/zzz.c", idiom);
    try harness.write("examples/x/aaa.h", idiom);
    _ = try harness.run(&[_][]const u8{"examples/x"});
    const c_at = std.mem.indexOf(u8, harness.stderr.items, "zzz.c").?;
    const h_at = std.mem.indexOf(u8, harness.stderr.items, "aaa.h").?;
    try testing.expect(c_at < h_at);
}

test "an undecodable file cannot abort the sweep" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    try harness.write("examples/x/bad.c", "\xff\xfe\x00 not utf-8\n");
    try harness.write("examples/x/pin.c", idiom);
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "examples/x/pin.c:1") != null);
}

test "the selftest holds in both directions over a tree that clears the floor" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"--selftest"}));
    try testing.expect(std.mem.indexOf(u8, harness.stdout.items, "all assertions held") != null);
    try testing.expect(std.mem.indexOf(u8, harness.stdout.items, "[ok] MUST FIRE") != null);
}

test "the selftest fails when the live sweep cannot clear the floor" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(5, clean);
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"--selftest"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "SELFTEST FAILED") != null);
    try testing.expect(std.mem.indexOf(u8, harness.stdout.items, "[FAIL] live sweep sees 5") != null);
}

test "--selftest anywhere in argv wins over the path list" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    try harness.write("examples/bad/src/main.c", idiom);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{
        "examples/bad/src/main.c",
        "--selftest",
    }));
}

test "the selftest proves the build-output exclusion, not just the matcher" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.populate(cli.file_floor, clean);
    _ = try harness.run(&[_][]const u8{"--selftest"});
    try testing.expect(std.mem.indexOf(
        u8,
        harness.stdout.items,
        "[ok] MUST NOT FIRE: an in-source build file is excluded from the scope",
    ) != null);
}

test "enumerateTargets keeps the real source and drops the build file" {
    var harness = try Harness.init();
    defer harness.deinit();
    var targets = try cli.enumerateTargets(
        harness.arena.allocator(),
        harness.tmp.dir,
        harness.root,
        &[_][]const u8{ "examples/x/build/gen.c", "examples/x/src/main.c" },
    );
    defer targets.deinit();
    try testing.expectEqual(@as(usize, 1), targets.items.len);
    try testing.expectEqualStrings("examples/x/src/main.c", targets.items[0].display);
}

test "a source far past any read ceiling still reports its hand-encoded pin" {
    // The scanned total counts a file whether or not it could be read, so a
    // size ceiling on the read would report a clean tree for a file nobody
    // read. Written sparse, so this costs neither disk nor runtime.
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.tmp.dir.makePath("examples/huge/src");
    var file = try harness.tmp.dir.createFile("examples/huge/src/main.c", .{});
    defer file.close();
    try file.seekTo(96 * 1024 * 1024);
    try file.writeAll(idiom);

    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"examples/huge/src/main.c"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "hand-encoded board pin(s)") != null);
    try testing.expect(std.mem.indexOf(u8, harness.stderr.items, "examples/huge/src/main.c") != null);
}
