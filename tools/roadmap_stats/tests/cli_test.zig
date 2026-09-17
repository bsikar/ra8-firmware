//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression suite for the argv membrane and the exit contract
//! of `roadmap_stats` (#858). The parser quirks live next door in
//! `internal_test.zig`; everything here is about what argv resolves to, what
//! status the tool exits with, which stream carries each line, and whether a
//! rewrite actually reaches the disk.

const std = @import("std");
const testing = std.testing;
const cli = @import("cli");

fn actionName(action: cli.Action) []const u8 {
    return switch (action) {
        .help => "help",
        .audit => "audit",
        .usage_error => "usage_error",
    };
}

fn parse(argv: []const []const u8) cli.Action {
    return cli.parseArgs(argv);
}

// -- contract literals -------------------------------------------------------

test "the tool name is the diagnostic prefix" {
    try testing.expectEqualStrings("roadmap_stats", cli.tool);
}

test "the usage line names every option" {
    try testing.expectEqualStrings(
        "usage: roadmap_stats [-h] [--check] [--roadmap ROADMAP] [--repo-root REPO_ROOT]",
        cli.usage_line,
    );
}

test "the default target is docs/ROADMAP.md" {
    try testing.expectEqualStrings("docs", cli.default_roadmap_components[0]);
    try testing.expectEqualStrings("ROADMAP.md", cli.default_roadmap_components[1]);
}

test "the long option set is exactly the four argparse knows" {
    try testing.expectEqual(@as(usize, 4), cli.long_options.len);
    try testing.expectEqualStrings("help", cli.long_options[0]);
    try testing.expectEqualStrings("check", cli.long_options[1]);
    try testing.expectEqualStrings("roadmap", cli.long_options[2]);
    try testing.expectEqualStrings("repo-root", cli.long_options[3]);
}

// -- parseArgs ---------------------------------------------------------------

test "no arguments is the rewriting mode" {
    const action = parse(&[_][]const u8{});
    try testing.expectEqualStrings("audit", actionName(action));
    try testing.expect(!action.audit.check);
    try testing.expect(action.audit.roadmap == null);
    try testing.expect(action.audit.repo_root == null);
}

test "--check selects check mode" {
    const action = parse(&[_][]const u8{"--check"});
    try testing.expect(action.audit.check);
}

test "--check abbreviates down to --c" {
    try testing.expect(parse(&[_][]const u8{"--che"}).audit.check);
    try testing.expect(parse(&[_][]const u8{"--ch"}).audit.check);
    try testing.expect(parse(&[_][]const u8{"--c"}).audit.check);
}

test "--roadmap takes the next argument" {
    const action = parse(&[_][]const u8{ "--roadmap", "/tmp/R.md" });
    try testing.expectEqualStrings("/tmp/R.md", action.audit.roadmap.?);
}

test "--roadmap accepts an inline value" {
    const action = parse(&[_][]const u8{"--roadmap=/tmp/R.md"});
    try testing.expectEqualStrings("/tmp/R.md", action.audit.roadmap.?);
}

test "--roadmap abbreviates to --ro" {
    const action = parse(&[_][]const u8{ "--ro", "R.md" });
    try testing.expectEqualStrings("R.md", action.audit.roadmap.?);
}

test "--repo-root takes the next argument and abbreviates to --rep" {
    const action = parse(&[_][]const u8{ "--rep", "/srv/repo" });
    try testing.expectEqualStrings("/srv/repo", action.audit.repo_root.?);
}

test "--r is AMBIGUOUS between --roadmap and --repo-root" {
    const action = parse(&[_][]const u8{ "--r", "x" });
    try testing.expectEqualStrings("usage_error", actionName(action));
    try testing.expectEqual(cli.UsageKind.ambiguous, action.usage_error.kind);
    try testing.expectEqualStrings("--r", action.usage_error.arg);
}

test "options combine in either order" {
    const first = parse(&[_][]const u8{ "--check", "--roadmap", "R.md" });
    try testing.expect(first.audit.check);
    try testing.expectEqualStrings("R.md", first.audit.roadmap.?);
    const second = parse(&[_][]const u8{ "--roadmap", "R.md", "--check" });
    try testing.expect(second.audit.check);
    try testing.expectEqualStrings("R.md", second.audit.roadmap.?);
}

test "a later value wins over an earlier one" {
    const action = parse(&[_][]const u8{ "--roadmap", "a", "--roadmap", "b" });
    try testing.expectEqualStrings("b", action.audit.roadmap.?);
}

test "-h and --help and their abbreviations ask for help" {
    try testing.expectEqualStrings("help", actionName(parse(&[_][]const u8{"-h"})));
    try testing.expectEqualStrings("help", actionName(parse(&[_][]const u8{"--help"})));
    try testing.expectEqualStrings("help", actionName(parse(&[_][]const u8{"--hel"})));
    try testing.expectEqualStrings("help", actionName(parse(&[_][]const u8{"--h"})));
}

test "help wins as soon as it is seen" {
    try testing.expectEqualStrings(
        "help",
        actionName(parse(&[_][]const u8{ "--check", "-h", "--nope" })),
    );
}

test "a store_true option rejects an explicit value" {
    const action = parse(&[_][]const u8{"--check=1"});
    try testing.expectEqual(cli.UsageKind.ignored_explicit_argument, action.usage_error.kind);
    try testing.expectEqualStrings("--check=1", action.usage_error.arg);
}

test "--help with an explicit value is a usage error, not help" {
    const action = parse(&[_][]const u8{"--help=1"});
    try testing.expectEqual(cli.UsageKind.ignored_explicit_argument, action.usage_error.kind);
}

test "a value option with nothing after it expects one argument" {
    const action = parse(&[_][]const u8{"--roadmap"});
    try testing.expectEqual(cli.UsageKind.expected_one_argument, action.usage_error.kind);
    try testing.expectEqualStrings("--roadmap", action.usage_error.arg);
}

test "a value option followed by another option expects one argument" {
    const action = parse(&[_][]const u8{ "--roadmap", "--check" });
    try testing.expectEqual(cli.UsageKind.expected_one_argument, action.usage_error.kind);
}

test "an empty inline value is accepted, as argparse accepts it" {
    const action = parse(&[_][]const u8{"--roadmap="});
    try testing.expectEqualStrings("", action.audit.roadmap.?);
}

test "an unknown long option is unrecognized" {
    const action = parse(&[_][]const u8{"--nope"});
    try testing.expectEqual(cli.UsageKind.unrecognized, action.usage_error.kind);
    try testing.expectEqualStrings("--nope", action.usage_error.arg);
}

test "an unknown short option is unrecognized" {
    try testing.expectEqual(
        cli.UsageKind.unrecognized,
        parse(&[_][]const u8{"-x"}).usage_error.kind,
    );
}

test "a lone dash is a positional, so unrecognized" {
    const action = parse(&[_][]const u8{"-"});
    try testing.expectEqual(cli.UsageKind.unrecognized, action.usage_error.kind);
    try testing.expectEqualStrings("-", action.usage_error.arg);
}

test "a positional argument is unrecognized, because none is declared" {
    const action = parse(&[_][]const u8{"ROADMAP.md"});
    try testing.expectEqual(cli.UsageKind.unrecognized, action.usage_error.kind);
    try testing.expectEqualStrings("ROADMAP.md", action.usage_error.arg);
}

test "a lone -- is consumed and leaves the rewriting mode" {
    const action = parse(&[_][]const u8{"--"});
    try testing.expectEqualStrings("audit", actionName(action));
    try testing.expect(!action.audit.check);
}

test "-- then an option makes that option a positional" {
    const action = parse(&[_][]const u8{ "--", "--check" });
    try testing.expectEqual(cli.UsageKind.unrecognized, action.usage_error.kind);
    try testing.expectEqualStrings("--check", action.usage_error.arg);
}

test "options before -- still apply" {
    const action = parse(&[_][]const u8{ "--check", "--" });
    try testing.expect(action.audit.check);
}

test "the first offending argument is the one reported" {
    const action = parse(&[_][]const u8{ "--nope", "--alsonope" });
    try testing.expectEqualStrings("--nope", action.usage_error.arg);
}

test "option names are case sensitive" {
    try testing.expectEqual(
        cli.UsageKind.unrecognized,
        parse(&[_][]const u8{"--CHECK"}).usage_error.kind,
    );
}

test "resolveLong resolves exactly, by unique prefix, and refuses the rest" {
    try testing.expect(cli.resolveLong("check") == .check);
    try testing.expect(cli.resolveLong("repo-root") == .repo_root);
    try testing.expect(cli.resolveLong("roa") == .roadmap);
    try testing.expect(cli.resolveLong("r") == .ambiguous);
    try testing.expect(cli.resolveLong("zzz") == .unknown);
    try testing.expect(cli.resolveLong("") == .unknown);
}

// -- path resolution ---------------------------------------------------------

test "an explicit --roadmap wins over every root" {
    const path = try cli.resolveRoadmapPath(
        testing.allocator,
        .{ .roadmap = "/tmp/x.md", .repo_root = "/srv" },
        "/env",
    );
    defer testing.allocator.free(path);
    try testing.expectEqualStrings("/tmp/x.md", path);
}

test "--repo-root wins over the environment" {
    const path = try cli.resolveRoadmapPath(testing.allocator, .{ .repo_root = "/srv" }, "/env");
    defer testing.allocator.free(path);
    try testing.expectEqualStrings("/srv/docs/ROADMAP.md", path);
}

test "the environment wins over the cwd" {
    const path = try cli.resolveRoadmapPath(testing.allocator, .{}, "/env");
    defer testing.allocator.free(path);
    try testing.expectEqualStrings("/env/docs/ROADMAP.md", path);
}

test "with no root at all the path is relative to the cwd" {
    // The root falls back to `"."`, which `std.fs.path.join` keeps as a
    // leading component, so the path reads `./docs/ROADMAP.md`. Pinned as
    // the tool's own behaviour: it is what the diagnostics print.
    const path = try cli.resolveRoadmapPath(testing.allocator, .{}, null);
    defer testing.allocator.free(path);
    try testing.expectEqualStrings("./docs/ROADMAP.md", path);
}

// -- run() over a real directory --------------------------------------------

const begin_mark = "<!-- BEGIN SUMMARY -- DO NOT EDIT BY HAND -- managed by roadmap_stats.py -->";
const end_mark = "<!-- END SUMMARY -->";

const body = "### ra8_gpio\n`[x]` Status: done\n```\n[x] one\n[ ] two\n```\n";

const current_summary = begin_mark ++ "\n" ++
    "- Total drivers tracked: 1\n" ++
    "- DONE:    1\n" ++
    "- WIP:     0\n" ++
    "- BLOCKED: 0\n" ++
    "- TODO:    0\n" ++
    "- Checklist coverage: 1/2 boxes ticked (50.0%)\n" ++
    end_mark;

const Outcome = struct {
    status: u8,
    out: []u8,
    err: []u8,

    fn deinit(self: Outcome) void {
        testing.allocator.free(self.out);
        testing.allocator.free(self.err);
    }
};

fn invoke(dir: std.fs.Dir, argv: []const []const u8) !Outcome {
    var out = std.ArrayList(u8).init(testing.allocator);
    errdefer out.deinit();
    var err = std.ArrayList(u8).init(testing.allocator);
    errdefer err.deinit();

    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    const status = try cli.run(
        arena.allocator(),
        dir,
        argv,
        null,
        out.writer(),
        err.writer(),
    );
    return .{
        .status = status,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

test "--help exits 0 on stdout and prints nothing to stderr" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const outcome = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--help" });
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 0), outcome.status);
    try testing.expect(std.mem.startsWith(u8, outcome.out, cli.usage_line));
    try testing.expectEqualStrings("", outcome.err);
}

test "a usage error exits 2, keeps stdout empty and names the argument" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const outcome = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--nope" });
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 2), outcome.status);
    try testing.expectEqualStrings("", outcome.out);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "unrecognized arguments: --nope") != null);
    try testing.expect(std.mem.startsWith(u8, outcome.err, cli.usage_line));
}

test "a missing roadmap exits 2 and says so" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const outcome = try invoke(
        tmp.dir,
        &[_][]const u8{ "roadmap_stats", "--roadmap", "absent.md", "--check" },
    );
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 2), outcome.status);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "not found: absent.md") != null);
}

test "a roadmap that is a directory exits 1 rather than a traceback" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makeDir("ROADMAP.md");
    const outcome = try invoke(
        tmp.dir,
        &[_][]const u8{ "roadmap_stats", "--roadmap", "ROADMAP.md" },
    );
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 1), outcome.status);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "cannot read") != null);
}

test "a roadmap that is not UTF-8 exits 1" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = "\xff\xfe not text" });
    const outcome = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md" });
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 1), outcome.status);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "not valid UTF-8") != null);
}

test "a roadmap with no markers exits 2 and writes nothing" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = body });
    const outcome = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md" });
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 2), outcome.status);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "missing the BEGIN/END SUMMARY markers") != null);

    const after = try tmp.dir.readFileAlloc(testing.allocator, "R.md", 4096);
    defer testing.allocator.free(after);
    try testing.expectEqualStrings(body, after);
}

test "an END before the BEGIN exits 2 and does NOT truncate the document" {
    // The predecessor would have written back everything up to the summary
    // and dropped the tail. This tool refuses instead.
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const inverted = "head\n" ++ end_mark ++ "\nmiddle\n" ++ begin_mark ++ "\nold\ntail\n";
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = inverted });
    const outcome = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md" });
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 2), outcome.status);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "END SUMMARY before BEGIN SUMMARY") != null);

    const after = try tmp.dir.readFileAlloc(testing.allocator, "R.md", 4096);
    defer testing.allocator.free(after);
    try testing.expectEqualStrings(inverted, after);
}

test "a current summary exits 0 with the unchanged census" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const document = body ++ "\n" ++ current_summary ++ "\ntail\n";
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = document });
    const outcome = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md" });
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 0), outcome.status);
    try testing.expectEqualStrings("", outcome.out);
    try testing.expect(std.mem.indexOf(
        u8,
        outcome.err,
        "unchanged (drivers=1 DONE=1 WIP=0 BLOCKED=0 TODO=0 boxes=1/2)",
    ) != null);
}

test "a current summary is reported unchanged in check mode too" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const document = body ++ "\n" ++ current_summary ++ "\n";
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = document });
    const outcome = try invoke(
        tmp.dir,
        &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md", "--check" },
    );
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 0), outcome.status);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "unchanged") != null);
}

test "a stale summary in check mode exits 1, names the refresh recipe and writes nothing" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const document = body ++ "\n" ++ begin_mark ++ "\nstale\n" ++ end_mark ++ "\ntail\n";
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = document });
    const outcome = try invoke(
        tmp.dir,
        &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md", "--check" },
    );
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 1), outcome.status);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "summary is stale") != null);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "just docs::record_stats") != null);

    const after = try tmp.dir.readFileAlloc(testing.allocator, "R.md", 4096);
    defer testing.allocator.free(after);
    try testing.expectEqualStrings(document, after);
}

test "a stale summary without --check is rewritten in place" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const document = body ++ "\n" ++ begin_mark ++ "\nstale\n" ++ end_mark ++ "\ntail\n";
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = document });
    const outcome = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md" });
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 0), outcome.status);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "rewrote summary (drivers=1") != null);

    const after = try tmp.dir.readFileAlloc(testing.allocator, "R.md", 8192);
    defer testing.allocator.free(after);
    try testing.expectEqualStrings(body ++ "\n" ++ current_summary ++ "\ntail\n", after);
}

test "the rewrite is idempotent: a second run reports unchanged" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const document = body ++ "\n" ++ begin_mark ++ "\nstale\n" ++ end_mark ++ "\ntail\n";
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = document });

    const first = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md" });
    defer first.deinit();
    try testing.expectEqual(@as(u8, 0), first.status);

    const second = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md" });
    defer second.deinit();
    try testing.expectEqual(@as(u8, 0), second.status);
    try testing.expect(std.mem.indexOf(u8, second.err, "unchanged") != null);

    const third = try invoke(
        tmp.dir,
        &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md", "--check" },
    );
    defer third.deinit();
    try testing.expectEqual(@as(u8, 0), third.status);
}

test "the rewrite preserves everything outside the markers byte for byte" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const document = "# Title\r\n\r\n" ++ body ++ begin_mark ++ "\nstale\n" ++ end_mark ++
        "\r\nappendix\ttabbed\r\n";
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = document });
    const outcome = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md" });
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 0), outcome.status);

    const after = try tmp.dir.readFileAlloc(testing.allocator, "R.md", 8192);
    defer testing.allocator.free(after);
    try testing.expect(std.mem.startsWith(u8, after, "# Title\r\n\r\n"));
    try testing.expect(std.mem.endsWith(u8, after, "\r\nappendix\ttabbed\r\n"));
}

test "a document with no drivers still rewrites to a zeroed summary" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const document = "prose only\n" ++ begin_mark ++ "\nwrong\n" ++ end_mark ++ "\n";
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = document });
    const outcome = try invoke(tmp.dir, &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md" });
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 0), outcome.status);

    const after = try tmp.dir.readFileAlloc(testing.allocator, "R.md", 4096);
    defer testing.allocator.free(after);
    try testing.expect(std.mem.indexOf(u8, after, "- Total drivers tracked: 0") != null);
    try testing.expect(std.mem.indexOf(u8, after, "0/0 boxes ticked (0.0%)") != null);
}

test "--repo-root finds docs/ROADMAP.md under that root" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("repo/docs");
    const document = body ++ "\n" ++ current_summary ++ "\n";
    try tmp.dir.writeFile(.{ .sub_path = "repo/docs/ROADMAP.md", .data = document });
    const outcome = try invoke(
        tmp.dir,
        &[_][]const u8{ "roadmap_stats", "--repo-root", "repo", "--check" },
    );
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 0), outcome.status);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "unchanged") != null);
}

test "an empty argv list is the rewriting mode, not a crash" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const outcome = try invoke(tmp.dir, &[_][]const u8{});
    defer outcome.deinit();
    // No roadmap under the temporary cwd, so the resolution reports exit 2.
    try testing.expectEqual(@as(u8, 2), outcome.status);
    try testing.expect(std.mem.indexOf(u8, outcome.err, "docs/ROADMAP.md") != null);
}

test "check mode never reports a stale summary as current" {
    // The gate's whole purpose: a document whose census disagrees with its
    // block must fail, and the failure must not be reachable by writing.
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const document = body ++ body ++ "\n" ++ current_summary ++ "\n";
    try tmp.dir.writeFile(.{ .sub_path = "R.md", .data = document });
    const outcome = try invoke(
        tmp.dir,
        &[_][]const u8{ "roadmap_stats", "--roadmap", "R.md", "--check" },
    );
    defer outcome.deinit();
    try testing.expectEqual(@as(u8, 1), outcome.status);
}
