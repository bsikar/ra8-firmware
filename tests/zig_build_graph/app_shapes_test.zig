//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! app_shapes.zig's own tests, and the two-direction check that gives the
//! module its point: the committed ledger against app_table.zig's eleven
//! cross-built apps.
//!
//! The ledger arrives as an @embedFile of an import name only the test module
//! declares (build.zig wires it), so these tests read the FILE that is
//! committed, not a copy of it, and cannot pass against a ledger that was
//! never generated -- parseLedger refuses an empty one, and the floors below
//! refuse a small one.
//!
//! What the two directions catch:
//!
//!   - A kind of app the tree grows and the cross-build table has never
//!     built, which today passes `zig build arm` in silence. It now has to be
//!     either cross-built or written down in app_shapes.uncovered with what
//!     adding it would cost.
//!   - An entry in that uncovered table that no declaration matches any more,
//!     which is how a to-do list rots into fiction. A stale entry fails.

const std = @import("std");

/// Both halves arrive through the `build_graph` module rather than by relative
/// import: build.zig already reaches app_shapes.zig and app_table.zig, and a
/// second path to the same file would put it in two modules at once.
const graph = @import("build_graph");
const app_shapes = graph.app_shapes;
const app_table = graph.cross_sources.app_table;

const Shape = app_shapes.Shape;
const Entry = app_shapes.Entry;
const LedgerError = app_shapes.LedgerError;
const default_board = app_shapes.default_board;
const default_stack_bytes = app_shapes.default_stack_bytes;
const parseListfile = app_shapes.parseListfile;
const parseLedger = app_shapes.parseLedger;
const renderLedger = app_shapes.renderLedger;
const distinctShapes = app_shapes.distinctShapes;
const countRows = app_shapes.countRows;
const countRefusals = app_shapes.countRefusals;
const rowsWithShape = app_shapes.rowsWithShape;
const uncoveredFor = app_shapes.uncoveredFor;
const keywordLike = app_shapes.keywordLike;
const keywordsFromCmake = app_shapes.keywordsFromCmake;

/// The committed ledger, as the file on disk.
const ledger_source = @embedFile("app_shape_ledger_source");

/// cmake/ra8_add_app.cmake, so the keyword enum is held to the function that
/// declares the keywords rather than to a memory of it.
const add_app_cmake_source = @embedFile("ra8_add_app_cmake_source");

/// Floors, so a parser that stops understanding the tree cannot report a
/// clean, smaller ledger. Measured at 234 declarations in 234 listfiles, 20
/// distinct shapes and no refusals; the floors sit below that, so ordinary
/// growth does not trip them while a collapse does.
const minimum_rows = 220;
const minimum_shapes = 18;
const minimum_listfiles = 220;

fn ledger(allocator: std.mem.Allocator) ![]Entry {
    return parseLedger(allocator, ledger_source);
}

/// The shape app_table.zig's own data says an app has. Reduced from the table
/// rather than from the listfile, which is the whole point: the two sides are
/// independent readings of the same declaration.
fn shapeOfTableApp(allocator: std.mem.Allocator, app: app_table.CrossApp) !Shape {
    const board_prefix = "libs/ra8_board_";
    const board = if (std.mem.startsWith(u8, app.board, board_prefix))
        app.board[board_prefix.len..]
    else
        app.board;
    return .{
        .board = board,
        .uses = try app_shapes.sortedCopy(allocator, app.uses),
        .no_nsc = app.no_nsc,
        .nsc_srcs = app.nsc_srcs.len != 0,
        .extra_srcs = app.extra_srcs.len != 0,
        .aux_srcs = app.aux_srcs.len != 0,
        .off_target_libs = app.off_target_libs.len != 0,
    };
}

fn rowNamed(entries: []const Entry, name: []const u8) ?app_shapes.Row {
    for (entries) |entry| switch (entry) {
        .row => |row| if (std.mem.eql(u8, row.name, name)) return row,
        .refusal => {},
    };
    return null;
}

test "the committed ledger parses, carries no refusals, and clears its floors" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const entries = try ledger(allocator);

    // A refusal in the committed ledger means the parser met a declaration it
    // could not read. That is a real finding about the tree, not a test to
    // relax: the reason is in the row.
    for (entries) |entry| switch (entry) {
        .refusal => |refusal| {
            std.debug.print(
                "ra8: the app-shape ledger refuses {s}: {s}\n",
                .{ refusal.listfile, refusal.reason },
            );
            return error.LedgerCarriesRefusals;
        },
        .row => {},
    };

    const rows = countRows(entries);
    try std.testing.expect(rows >= minimum_rows);
    try std.testing.expectEqual(@as(usize, 0), countRefusals(entries));

    const shapes = try distinctShapes(allocator, entries);
    try std.testing.expect(shapes.len >= minimum_shapes);

    var listfiles = std.StringHashMap(void).init(allocator);
    for (entries) |entry| try listfiles.put(entry.listfile(), {});
    try std.testing.expect(listfiles.count() >= minimum_listfiles);

    // Every row sits under one of the trees the ledger claims to cover, so a
    // ledger generated from somewhere else cannot satisfy the floors above.
    for (entries) |entry| {
        var under_a_root = false;
        for (app_shapes.roots) |tree| {
            if (std.mem.startsWith(u8, entry.listfile(), tree)) under_a_root = true;
        }
        try std.testing.expect(under_a_root);
    }
}

test "every cross-built app is in the ledger with the shape its table row declares" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const entries = try ledger(allocator);

    for (app_table.cross_apps) |app| {
        const row = rowNamed(entries, app.name) orelse {
            std.debug.print("ra8: cross-built app {s} is not in the ledger\n", .{app.name});
            return error.CrossAppMissingFromLedger;
        };
        // The listfile the ledger read it out of is the app's own, which is
        // the cheap check that the two sides are talking about one app.
        try std.testing.expect(std.mem.startsWith(u8, row.listfile, app.dir));

        const declared = try shapeOfTableApp(allocator, app);
        if (!declared.eql(row.shape)) {
            std.debug.print("ra8: {s} table shape {s} but listfile shape {s}\n", .{
                app.name,
                try declared.label(allocator),
                try row.shape.label(allocator),
            });
            return error.CrossAppShapeDisagrees;
        }
        try std.testing.expectEqual(app.stack_bytes, row.stack_bytes);
    }
}

test "every kind of app in the tree is either cross-built or written down as uncovered" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const entries = try ledger(allocator);

    var built = std.ArrayList(Shape).init(allocator);
    for (app_table.cross_apps) |app| try built.append(try shapeOfTableApp(allocator, app));

    var covered_rows: usize = 0;
    var uncovered_rows: usize = 0;
    for (try distinctShapes(allocator, entries)) |shape| {
        var cross_built = false;
        for (built.items) |candidate| {
            if (candidate.eql(shape)) cross_built = true;
        }
        const here = rowsWithShape(entries, shape);
        if (cross_built) {
            covered_rows += here;
            continue;
        }
        if (uncoveredFor(shape) == null) {
            std.debug.print(
                "ra8: {d} declarations have shape {s}, which no table app cross-builds and " ++
                    "app_shapes.uncovered does not name\n",
                .{ here, try shape.label(allocator) },
            );
            return error.UnaccountedAppShape;
        }
        uncovered_rows += here;
    }

    // Arithmetic, so a shape cannot be counted on both sides: every row is in
    // exactly one of the two buckets.
    try std.testing.expectEqual(countRows(entries), covered_rows + uncovered_rows);
    // And the table is still the majority of the tree, which is the claim
    // app_table.zig's prose makes.
    try std.testing.expect(covered_rows > uncovered_rows);
}

test "no entry in the uncovered table has gone stale" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const entries = try ledger(allocator);

    for (app_shapes.uncovered) |entry| {
        const matching = rowsWithShape(entries, entry.shape);
        if (matching == 0) {
            std.debug.print(
                "ra8: app_shapes.uncovered names {s} ({s}), which no declaration has any more\n",
                .{ entry.representative, try entry.shape.label(allocator) },
            );
            return error.StaleUncoveredEntry;
        }
        // The representative is a real app with that exact shape, so the
        // cheapest-to-add suggestion points at something that exists.
        const row = rowNamed(entries, entry.representative) orelse {
            std.debug.print("ra8: uncovered representative {s} is not an app\n", .{entry.representative});
            return error.UnknownRepresentative;
        };
        try std.testing.expect(row.shape.eql(entry.shape));
        try std.testing.expect(entry.note.len > 0);
    }

    // No two entries describe the same kind of app.
    for (app_shapes.uncovered, 0..) |entry, i| {
        for (app_shapes.uncovered[i + 1 ..]) |other| {
            try std.testing.expect(!entry.shape.eql(other.shape));
        }
    }
}

test "the keyword enum is the keyword set cmake/ra8_add_app.cmake declares" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const declared = try keywordsFromCmake(allocator, add_app_cmake_source) orelse {
        std.debug.print("ra8: cmake/ra8_add_app.cmake has no cmake_parse_arguments() call to read\n", .{});
        return error.NoKeywordDeclaration;
    };

    const groups = [_]struct { group: app_shapes.Keyword.Group, names: []const []const u8 }{
        .{ .group = .option, .names = declared.options },
        .{ .group = .one_value, .names = declared.one_value },
        .{ .group = .multi_value, .names = declared.multi_value },
    };

    var total: usize = 0;
    for (groups) |group| {
        total += group.names.len;
        for (group.names) |name| {
            const keyword = app_shapes.keywordOf(name) orelse {
                std.debug.print("ra8: ra8_add_app() accepts {s}, which app_shapes.Keyword has no arm for\n", .{name});
                return error.UnmodelledKeyword;
            };
            try std.testing.expectEqual(group.group, keyword.group());
        }
    }
    // Both directions: no arm of the enum is a keyword the function stopped
    // accepting.
    try std.testing.expectEqual(std.enums.values(app_shapes.Keyword).len, total);
}

const fixture =
    \\# A commented-out call, which must not be read as a declaration:
    \\# ra8_add_app(NAME ghost_app)
    \\ra8_add_app(
    \\  NAME blink_fixture
    \\  DESCRIPTION "two words and a (paren)"
    \\  LIBS ra8_board_ek_ra8d2 ra8_io_bus
    \\  STACK_BYTES 4096
    \\)
    \\
    \\ra8_add_app(
    \\  NAME tz_fixture
    \\  BOARD ra8p1
    \\  USES usbx threadx  # order must not matter
    \\  NSC_SRCS ra8_nsc_cgc.c
    \\  AUX_SRCS src/cpu1_main.c
    \\  NO_NSC
    \\)
    \\
;

test "the block iterator reads real calls and skips commented ones" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const entries = try parseListfile(arena.allocator(), "fixture/CMakeLists.txt", fixture);
    try std.testing.expectEqual(@as(usize, 2), entries.len);
    try std.testing.expectEqualStrings("blink_fixture", entries[0].row.name);
    try std.testing.expectEqualStrings("tz_fixture", entries[1].row.name);
}

test "a declaration reduces to its shape, defaults included" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const entries = try parseListfile(arena.allocator(), "fixture/CMakeLists.txt", fixture);

    const blink = entries[0].row;
    try std.testing.expectEqualStrings(default_board, blink.shape.board);
    try std.testing.expectEqual(@as(usize, 0), blink.shape.uses.len);
    try std.testing.expectEqual(@as(u32, 4096), blink.stack_bytes);
    // LIBS is not part of a shape, so naming two libraries leaves every flag
    // clear: this app is the same KIND as an app that names none.
    try std.testing.expect(blink.shape.eql(.{ .board = default_board }));

    const tz = entries[1].row;
    try std.testing.expectEqualStrings("ra8p1", tz.shape.board);
    // USES is sorted, so the fixture's `usbx threadx` compares equal to an
    // app that says `threadx usbx`.
    try std.testing.expect(tz.shape.eql(.{
        .board = "ra8p1",
        .uses = &.{ "threadx", "usbx" },
        .nsc_srcs = true,
        .aux_srcs = true,
        .no_nsc = true,
    }));
    // STACK_BYTES unnamed means 2200, which is ra8_add_app()'s own default and
    // not cmake/ra8_warnings.cmake's 2048.
    try std.testing.expectEqual(default_stack_bytes, tz.stack_bytes);
}

test "a keyword-like token nobody declares is refused, not filed as a value" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const entries = try parseListfile(
        arena.allocator(),
        "fixture/CMakeLists.txt",
        "ra8_add_app(NAME x STACK_BYTES 2200 FUTURE_KEYWORD y)",
    );
    try std.testing.expectEqual(@as(usize, 1), entries.len);
    try std.testing.expect(std.mem.indexOf(u8, entries[0].refusal.reason, "FUTURE_KEYWORD") != null);
}

test "a declaration with no NAME, and a STACK_BYTES that is not a number, are refused" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const nameless = try parseListfile(arena.allocator(), "f/CMakeLists.txt", "ra8_add_app(LIBS ra8_io)");
    try std.testing.expectEqualStrings("declaration names no NAME", nameless[0].refusal.reason);

    const bad_stack = try parseListfile(
        arena.allocator(),
        "f/CMakeLists.txt",
        "ra8_add_app(NAME x STACK_BYTES ${SOME_VAR})",
    );
    try std.testing.expect(std.mem.indexOf(u8, bad_stack[0].refusal.reason, "not a number") != null);

    const orphan = try parseListfile(arena.allocator(), "f/CMakeLists.txt", "ra8_add_app(stray NAME x)");
    try std.testing.expect(std.mem.indexOf(u8, orphan[0].refusal.reason, "before any keyword") != null);
}

test "the ledger round-trips both kinds of entry" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const entries = [_]Entry{
        .{ .row = .{
            .name = "tz_fixture",
            .listfile = "fixture/CMakeLists.txt",
            .shape = .{ .board = "ra8p1", .uses = &.{ "threadx", "usbx" }, .nsc_srcs = true, .aux_srcs = true },
            .stack_bytes = 32768,
        } },
        .{ .row = .{ .name = "bare", .listfile = "b/CMakeLists.txt", .shape = .{} } },
        .{ .refusal = .{ .listfile = "c/CMakeLists.txt", .reason = "declaration names no NAME" } },
    };
    const text = try renderLedger(allocator, &entries);
    const read_back = try parseLedger(allocator, text);
    try std.testing.expectEqual(entries.len, read_back.len);
    try std.testing.expectEqualStrings("tz_fixture", read_back[0].row.name);
    try std.testing.expect(read_back[0].row.shape.eql(entries[0].row.shape));
    try std.testing.expectEqual(@as(u32, 32768), read_back[0].row.stack_bytes);
    try std.testing.expect(read_back[1].row.shape.eql(.{}));
    try std.testing.expectEqualStrings("declaration names no NAME", read_back[2].refusal.reason);
    try std.testing.expectEqual(@as(usize, 2), countRows(&entries));
    try std.testing.expectEqual(@as(usize, 1), countRefusals(&entries));
}

test "the ledger reader refuses what it cannot read, and refuses being empty" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try std.testing.expectError(LedgerError.EmptyLedger, parseLedger(allocator, "# only a comment\n"));
    try std.testing.expectError(LedgerError.UnknownKind, parseLedger(allocator, "app_maybe\tx\ty\n"));
    try std.testing.expectError(
        LedgerError.WrongFieldCount,
        parseLedger(allocator, "app\tx\ty\tek_ra8d2\t-\t-\n"),
    );
    try std.testing.expectError(
        LedgerError.UnknownFlag,
        parseLedger(allocator, "app\tx\ty\tek_ra8d2\t-\tno_such_flag\t2200\n"),
    );
    try std.testing.expectError(
        LedgerError.BadStackBytes,
        parseLedger(allocator, "app\tx\ty\tek_ra8d2\t-\t-\tplenty\n"),
    );
}

test "a shape's label names its board, its middleware and its flags" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const bare = try (Shape{}).label(allocator);
    try std.testing.expectEqualStrings("board=ek_ra8d2 uses=- flags=-", bare);
    const full = try (Shape{
        .board = "ra8p1",
        .uses = &.{ "threadx", "usbx" },
        .extra_srcs = true,
        .aux_srcs = true,
    }).label(allocator);
    try std.testing.expectEqualStrings("board=ra8p1 uses=threadx,usbx flags=extra_srcs,aux_srcs", full);
}

test "distinct shapes collapse duplicates and keep genuinely different kinds apart" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const entries = [_]Entry{
        .{ .row = .{ .name = "a", .listfile = "a", .shape = .{} } },
        .{ .row = .{ .name = "b", .listfile = "b", .shape = .{} } },
        .{ .row = .{ .name = "c", .listfile = "c", .shape = .{ .extra_srcs = true } } },
        .{ .refusal = .{ .listfile = "d", .reason = "whatever" } },
    };
    const shapes = try distinctShapes(arena.allocator(), &entries);
    try std.testing.expectEqual(@as(usize, 2), shapes.len);
    try std.testing.expectEqual(@as(usize, 2), rowsWithShape(&entries, .{}));
    try std.testing.expectEqual(@as(usize, 1), rowsWithShape(&entries, .{ .extra_srcs = true }));
}

test "the uncovered table is keyed by shape, not by name" {
    const usbx = uncoveredFor(.{ .uses = &.{ "threadx", "usbx" } }) orelse return error.TestUnexpectedResult;
    try std.testing.expectEqualStrings("usb_selftest_wlun", usbx.representative);
    // The same middleware plus EXTRA_SRCS is a DIFFERENT kind of app, and has
    // its own entry.
    const dfu = uncoveredFor(.{ .uses = &.{ "threadx", "usbx" }, .extra_srcs = true }) orelse
        return error.TestUnexpectedResult;
    try std.testing.expectEqualStrings("dfu_bootloader", dfu.representative);
    // A shape the table cross-builds is not in here at all.
    try std.testing.expect(uncoveredFor(.{}) == null);
}

test "keywordLike catches a new keyword and leaves ordinary values alone" {
    try std.testing.expect(keywordLike("FUTURE_KEYWORD"));
    try std.testing.expect(keywordLike("USES"));
    try std.testing.expect(!keywordLike("2200"));
    try std.testing.expect(!keywordLike("src/main.c"));
    try std.testing.expect(!keywordLike("${CMAKE_CURRENT_SOURCE_DIR}/x.c"));
    try std.testing.expect(!keywordLike("ra8_io_bus"));
}

test "the keyword lists come out of the cmake source, and a missing call is null" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const parsed = try keywordsFromCmake(allocator,
        \\function(ra8_add_app)
        \\  cmake_parse_arguments(
        \\    _RA8_APP
        \\    "NO_NSC"
        \\    "NAME;STACK_BYTES"
        \\    "USES;LIBS"
        \\    ${ARGN}
        \\  )
        \\endfunction()
    ) orelse return error.TestUnexpectedResult;
    try std.testing.expectEqual(@as(usize, 1), parsed.options.len);
    try std.testing.expectEqualStrings("NO_NSC", parsed.options[0]);
    try std.testing.expectEqual(@as(usize, 2), parsed.one_value.len);
    try std.testing.expectEqualStrings("STACK_BYTES", parsed.one_value[1]);
    try std.testing.expectEqualStrings("LIBS", parsed.multi_value[1]);
    try std.testing.expect(try keywordsFromCmake(allocator, "function(other)\nendfunction()") == null);
}
