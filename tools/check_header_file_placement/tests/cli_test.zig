//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Regression tests for the argv membrane and the exit contract (#858, #1219).
//! Each case drives `cli.run` over a real temporary tree, so enumeration,
//! exclusion and the reported status are exercised together exactly as
//! `scripts/builders/check_header_file_placement.sh` invokes them.

const std = @import("std");
const cli = @import("cli");

const testing = std.testing;

const Harness = struct {
    tmp: std.testing.TmpDir,
    arena: std.heap.ArenaAllocator,
    root: []const u8,
    out: std.ArrayList(u8),
    err: std.ArrayList(u8),

    fn init() !Harness {
        var tmp = std.testing.tmpDir(.{});
        errdefer tmp.cleanup();
        var arena = std.heap.ArenaAllocator.init(testing.allocator);
        const scratch = arena.allocator();
        const real_root = try tmp.dir.realpathAlloc(scratch, ".");
        return .{
            .tmp = tmp,
            .arena = arena,
            .root = real_root,
            .out = std.ArrayList(u8).init(testing.allocator),
            .err = std.ArrayList(u8).init(testing.allocator),
        };
    }

    fn deinit(self: *Harness) void {
        self.out.deinit();
        self.err.deinit();
        self.arena.deinit();
        self.tmp.cleanup();
    }

    fn allocator(self: *Harness) std.mem.Allocator {
        return self.arena.allocator();
    }

    fn write(self: *Harness, relative: []const u8) !void {
        if (std.fs.path.dirname(relative)) |parent| try self.tmp.dir.makePath(parent);
        var file = try self.tmp.dir.createFile(relative, .{ .truncate = true });
        defer file.close();
        try file.writeAll("#pragma once\n");
    }

    fn mkdir(self: *Harness, relative: []const u8) !void {
        try self.tmp.dir.makePath(relative);
    }

    fn absolute(self: *Harness, relative: []const u8) ![]const u8 {
        return std.fmt.allocPrint(self.allocator(), "{s}/{s}", .{ self.root, relative });
    }

    fn run(self: *Harness, argv: []const []const u8) !u8 {
        const host = std.fs.cwd();
        return cli.run(
            self.allocator(),
            host,
            self.root,
            self.root,
            argv,
            self.out.writer(),
            self.err.writer(),
        );
    }

    fn stdout(self: *Harness) []const u8 {
        return self.out.items;
    }

    fn stderr(self: *Harness) []const u8 {
        return self.err.items;
    }
};

/// A tree that clears the whole-tree floor: enough private headers under
/// libs/**/src to satisfy MIN_PRIVATE_HEADERS, all of them `_internal`.
fn seedFloor(harness: *Harness) !void {
    var index: usize = 0;
    while (index < cli.implementation.min_private_headers) : (index += 1) {
        var buffer: [96]u8 = undefined;
        const path = try std.fmt.bufPrint(
            &buffer,
            "libs/module{d}/src/widget_internal.h",
            .{index},
        );
        try harness.write(path);
    }
}

// ---------------------------------------------------------------------------
// argument parsing
// ---------------------------------------------------------------------------

test "no arguments means the whole-tree sweep" {
    const parsed = try cli.parseArgs(testing.allocator, &[_][]const u8{});
    defer testing.allocator.free(parsed.ok.paths);
    try testing.expect(!parsed.ok.selftest);
    try testing.expectEqual(@as(usize, 0), parsed.ok.paths.len);
}

test "--selftest sets the flag" {
    const parsed = try cli.parseArgs(testing.allocator, &[_][]const u8{"--selftest"});
    defer testing.allocator.free(parsed.ok.paths);
    try testing.expect(parsed.ok.selftest);
}

test "argparse abbreviates the only long option" {
    for ([_][]const u8{ "--s", "--se", "--self", "--selftes" }) |spelling| {
        const parsed = try cli.parseArgs(testing.allocator, &[_][]const u8{spelling});
        defer testing.allocator.free(parsed.ok.paths);
        try testing.expect(parsed.ok.selftest);
    }
}

test "a path list is collected in order" {
    const parsed = try cli.parseArgs(testing.allocator, &[_][]const u8{ "b.h", "a.h" });
    defer testing.allocator.free(parsed.ok.paths);
    try testing.expectEqualStrings("b.h", parsed.ok.paths[0]);
    try testing.expectEqualStrings("a.h", parsed.ok.paths[1]);
}

test "--selftest may appear beside paths, and the refusal is the gate's own" {
    const parsed = try cli.parseArgs(testing.allocator, &[_][]const u8{ "--selftest", "a.h" });
    defer testing.allocator.free(parsed.ok.paths);
    try testing.expect(parsed.ok.selftest);
    try testing.expectEqual(@as(usize, 1), parsed.ok.paths.len);
}

test "-- ends option parsing" {
    const parsed = try cli.parseArgs(testing.allocator, &[_][]const u8{ "--", "--selftest" });
    defer testing.allocator.free(parsed.ok.paths);
    try testing.expect(!parsed.ok.selftest);
    try testing.expectEqualStrings("--selftest", parsed.ok.paths[0]);
}

test "a bare dash is a positional" {
    const parsed = try cli.parseArgs(testing.allocator, &[_][]const u8{"-"});
    defer testing.allocator.free(parsed.ok.paths);
    try testing.expectEqualStrings("-", parsed.ok.paths[0]);
}

test "an unknown long option is unrecognised" {
    const parsed = try cli.parseArgs(testing.allocator, &[_][]const u8{"--bogus"});
    try testing.expectEqualStrings("--bogus", parsed.unrecognized);
}

test "an unknown short option is unrecognised" {
    const parsed = try cli.parseArgs(testing.allocator, &[_][]const u8{"-x"});
    try testing.expectEqualStrings("-x", parsed.unrecognized);
}

test "-h and --help ask for the usage" {
    try testing.expect(try cli.parseArgs(testing.allocator, &[_][]const u8{"-h"}) == .help);
    try testing.expect(try cli.parseArgs(testing.allocator, &[_][]const u8{"--help"}) == .help);
}

// ---------------------------------------------------------------------------
// exit statuses
// ---------------------------------------------------------------------------

test "an unrecognised option exits 2 with the usage line on stderr" {
    var harness = try Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 2), try harness.run(&[_][]const u8{"--bogus"}));
    try testing.expect(std.mem.startsWith(u8, harness.stderr(), cli.usage_line));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "unrecognized arguments: --bogus") != null);
    try testing.expectEqualStrings("", harness.stdout());
}

test "--selftest with paths exits 2 and refuses on stderr" {
    var harness = try Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 2), try harness.run(&[_][]const u8{ "--selftest", "a.h" }));
    try testing.expectEqualStrings("--selftest does not accept paths\n", harness.stderr());
}

test "--help exits 0 on stdout" {
    var harness = try Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"--help"}));
    try testing.expect(std.mem.startsWith(u8, harness.stdout(), cli.usage_line));
}

test "--selftest passes on its own fixture tree" {
    var harness = try Harness.init();
    defer harness.deinit();
    // The fixture tree goes under /tmp, where the real run puts it. The test
    // tmpdir lives inside .zig-cache, which the shared build-output rule
    // excludes, so a fixture built there would be filtered out before it was
    // ever audited.
    var seed: [8]u8 = undefined;
    std.crypto.random.bytes(&seed);
    const scratch = try std.fmt.allocPrint(
        harness.allocator(),
        "/tmp/ra8-hfp-selftest-{x}",
        .{std.mem.readInt(u64, &seed, .little)},
    );
    const host = std.fs.cwd();
    defer host.deleteTree(scratch) catch {};
    const status = try cli.runSelftest(
        harness.allocator(),
        host,
        "/nonexistent-repo-root",
        scratch,
        harness.out.writer(),
        harness.err.writer(),
    );
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expect(std.mem.indexOf(u8, harness.stdout(), "PASS (fire, quiet, tests, exclusions)") != null);
    try testing.expectEqualStrings("", harness.stderr());
}

test "a clean whole-tree sweep above the floor exits 0" {
    var harness = try Harness.init();
    defer harness.deinit();
    try seedFloor(&harness);
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stdout(), "all module-private (*_internal.h).") != null);
}

test "the clean line reports the private count, not the file count" {
    var harness = try Harness.init();
    defer harness.deinit();
    try seedFloor(&harness);
    // Public headers and headers outside any inc/src are enumerated but never
    // counted.
    try harness.write("libs/extra/inc/public.h");
    try harness.write("libs/extra/loose.h");
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stdout(), ": 100 src/ header(s) scanned") != null);
}

test "a collapsed whole-tree sweep exits 1 rather than reporting clean" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/src/widget_internal.h");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "below floor 100") != null);
    try testing.expectEqualStrings("", harness.stdout());
}

test "an empty tree exits 1, never a vacuous pass" {
    var harness = try Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "reached only 0 private header(s)") != null);
}

test "a misplaced header in the whole-tree sweep exits 1 with the table" {
    var harness = try Harness.init();
    defer harness.deinit();
    try seedFloor(&harness);
    try harness.write("libs/bad/src/widget.h");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "1 src/ header(s) are not *_internal.h") != null);
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "  libs/bad/src/widget.h\n") != null);
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "placement is the contract.") != null);
}

test "an explicit offender exits 1 without needing the floor" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/src/widget.h");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"libs/m/src/widget.h"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "libs/m/src/widget.h") != null);
}

test "an explicit clean file exits 0 without needing the floor" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/src/widget_internal.h");
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"libs/m/src/widget_internal.h"}));
    try testing.expect(std.mem.indexOf(u8, harness.stdout(), ": 1 src/ header(s) scanned") != null);
}

test "an explicit list that filters to nothing exits 0 with a note" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/src/widget.c");
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"libs/m/src/widget.c"}));
    try testing.expectEqualStrings("check_header_file_placement.py: no headers to scan\n", harness.stderr());
    try testing.expectEqualStrings("", harness.stdout());
}

test "an explicit list of only public headers scans zero and passes" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/inc/public.h");
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"libs/m/inc/public.h"}));
    try testing.expect(std.mem.indexOf(u8, harness.stdout(), ": 0 src/ header(s) scanned") != null);
}

// ---------------------------------------------------------------------------
// enumeration
// ---------------------------------------------------------------------------

test "a named directory is expanded recursively" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/src/a.h");
    try harness.write("libs/m/src/deep/b.hpp");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"libs/m"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "libs/m/src/a.h") != null);
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "libs/m/src/deep/b.hpp") != null);
}

test "a dot-prefixed header is NOT hidden from the walk" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/src/.hidden.h");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"libs/m"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), ".hidden.h") != null);
}

test "a DIRECTORY named like a header is scanned, as rglob returned it" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.mkdir("libs/m/src/generated.h");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"libs/m"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "libs/m/src/generated.h") != null);
}

test "a nonexistent header-suffixed path is still audited" {
    var harness = try Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"libs/ghost/src/missing.h"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "libs/ghost/src/missing.h") != null);
}

test "a nonexistent non-header path is dropped" {
    var harness = try Harness.init();
    defer harness.deinit();
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"libs/ghost/src/missing.c"}));
    try testing.expectEqualStrings("check_header_file_placement.py: no headers to scan\n", harness.stderr());
}

test "an absolute path is taken as given" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/src/widget.h");
    const path = try harness.absolute("libs/m/src/widget.h");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{path}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "libs/m/src/widget.h") != null);
}

test "only the six scan roots are swept" {
    var harness = try Harness.init();
    defer harness.deinit();
    try seedFloor(&harness);
    // scripts/ is not a scan root, so an offender there is invisible.
    try harness.write("scripts/m/src/widget.h");
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
}

test "each scan root is actually walked" {
    var harness = try Harness.init();
    defer harness.deinit();
    try seedFloor(&harness);
    for (cli.implementation.scan_roots) |root| {
        var buffer: [96]u8 = undefined;
        const path = try std.fmt.bufPrint(&buffer, "{s}/m/src/offender.h", .{root});
        try harness.write(path);
    }
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "6 src/ header(s) are not") != null);
}

test "each header suffix is enumerated" {
    var harness = try Harness.init();
    defer harness.deinit();
    for (cli.implementation.header_suffixes) |suffix| {
        var buffer: [96]u8 = undefined;
        const path = try std.fmt.bufPrint(&buffer, "libs/m/src/widget{s}", .{suffix});
        try harness.write(path);
    }
    try harness.write("libs/m/src/widget.hxxx");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"libs/m"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "4 src/ header(s) are not") != null);
}

test "vendored and generated trees never reach the audit" {
    var harness = try Harness.init();
    defer harness.deinit();
    try seedFloor(&harness);
    try harness.write("libs/third_party/v/src/public.h");
    try harness.write("apps/shared_libs/third_party/v/src/public.h");
    try harness.write("libs/ra8_fonts/src/generated.h");
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
}

test "build output never reaches the audit" {
    var harness = try Harness.init();
    defer harness.deinit();
    try seedFloor(&harness);
    try harness.write("tests/m/build/src/generated.h");
    try harness.write("tools/m/CMakeFiles/src/generated.h");
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
}

test "a source directory called build under libs stays visible" {
    var harness = try Harness.init();
    defer harness.deinit();
    try seedFloor(&harness);
    // libs is not a build-tree root, so this is first-party source.
    try harness.write("libs/m/build/src/widget.h");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "libs/m/build/src/widget.h") != null);
}

test "a nested inc rescues a header under a higher src" {
    var harness = try Harness.init();
    defer harness.deinit();
    try seedFloor(&harness);
    try harness.write("libs/m/src/sub/inc/public.h");
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{}));
}

test "offenders are listed in code-point order" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/z/src/z.h");
    try harness.write("libs/a/src/a.h");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{ "libs/z", "libs/a" }));
    const first = std.mem.indexOf(u8, harness.stderr(), "libs/a/src/a.h").?;
    const second = std.mem.indexOf(u8, harness.stderr(), "libs/z/src/z.h").?;
    try testing.expect(first < second);
}

test "findings go to stderr and leave stdout empty" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/src/widget.h");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"libs/m"}));
    try testing.expectEqualStrings("", harness.stdout());
}

test "a sweep reports a file named exactly .h under src/, as rglob did" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/src/.h");
    try testing.expectEqual(@as(u8, 1), try harness.run(&[_][]const u8{"libs/m"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "libs/m/src/.h") != null);
}

test "an explicitly named .h file is dropped, as _is_header did" {
    var harness = try Harness.init();
    defer harness.deinit();
    try harness.write("libs/m/src/.h");
    try testing.expectEqual(@as(u8, 0), try harness.run(&[_][]const u8{"libs/m/src/.h"}));
    try testing.expect(std.mem.indexOf(u8, harness.stderr(), "no headers to scan") != null);
}
