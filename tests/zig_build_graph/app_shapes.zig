//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What SHAPE every `ra8_add_app()` declaration in the tree has, read out of
//! the listfile that makes it.
//!
//! app_table.zig cross-builds eleven apps and says, in prose, that between
//! them they take both arms of every rule `ra8_add_app()` implements. Nothing
//! held that claim to the tree. There are 234 `ra8_add_app()` declarations
//! under `examples/` and `apps/`, and no file said how many distinct KINDS of
//! app that is, nor which kinds the table has never cross-built. A table that
//! covers eight kinds out of twenty is a fine place to be mid-migration; a
//! table that covers eight kinds and cannot say so is how the ARM step starts
//! passing while the graph has never seen a USBX app.
//!
//! So: parse the declarations, reduce each to a shape (its board, the
//! middleware it names in `USES`, and which of the source-set keywords it
//! takes), and commit the result as a ledger beside this file. `zig build
//! shapes` regenerates it from the tree and diffs, so a new kind of app fails
//! the step that claims to know the app tree instead of passing quietly.
//!
//! Three rules this module holds itself to.
//!
//! It REFUSES rather than guesses. A declaration it cannot read becomes a
//! refusal row naming the listfile and the reason, never a silently dropped
//! app, and the ledger carries the refusals so a parser that stops
//! understanding the tree cannot report a clean, smaller ledger.
//!
//! The keyword set is not lore. `keywordsFromCmake` extracts it from the
//! `cmake_parse_arguments()` call in cmake/ra8_add_app.cmake, and
//! app_shapes_test.zig holds the `Keyword` enum below to it, so a keyword
//! added there and not here is a failing test rather than a value this parser
//! silently files under nothing.
//!
//! `LIBS` is deliberately NOT part of a shape. Every app names a different
//! set, so folding it in would turn 234 declarations into nearly as many
//! shapes and say nothing. What a shape distinguishes is which ARMS of
//! `ra8_add_app()`'s own
//! decisions an app takes: the board opt-in gate (`BOARD`), the vendored
//! middleware that rewrites the app's flags (`USES`), the three-way NSC source
//! decision (`NSC_SRCS` / `NO_NSC` / neither), and whether the app compiles
//! helper or sibling-image translation units (`EXTRA_SRCS`, `AUX_SRCS`,
//! `OFF_TARGET_LIBS`). `libraries` is carried by app_table.zig, which is where
//! the gate that reads it lives.

const std = @import("std");

/// The committed ledger, repo-relative. `zig build shapes` diffs the tree
/// against this file.
pub const ledger_path = "tests/zig_build_graph/app_shape_ledger.tsv";

/// The trees the ledger covers. All eleven apps app_table.zig cross-builds
/// live under these two; the `ra8_add_app()` calls under `libs/` and `port/`
/// belong to test fixtures and a port shim, and no cross-build table claims
/// them.
pub const roots = [_][]const u8{ "examples", "apps" };

/// The `BOARD` an app gets by saying nothing (cmake/ra8_add_app.cmake).
pub const default_board = "ek_ra8d2";

/// The `STACK_BYTES` an app gets by saying nothing. 2200, not the 2048
/// cmake/ra8_warnings.cmake falls back to: `ra8_add_app()` always forwards the
/// keyword, so its own default is what an app gets.
pub const default_stack_bytes: u32 = 2200;

/// Every keyword `ra8_add_app()` accepts. Held against the listfile that
/// declares them by app_shapes_test.zig.
pub const Keyword = enum {
    no_nsc,
    name,
    stack_bytes,
    description,
    board,
    uses,
    libs,
    off_target_libs,
    nsc_srcs,
    extra_srcs,
    aux_srcs,

    /// How cmake/ra8_add_app.cmake spells it.
    pub fn spelling(self: Keyword) []const u8 {
        return switch (self) {
            .no_nsc => "NO_NSC",
            .name => "NAME",
            .stack_bytes => "STACK_BYTES",
            .description => "DESCRIPTION",
            .board => "BOARD",
            .uses => "USES",
            .libs => "LIBS",
            .off_target_libs => "OFF_TARGET_LIBS",
            .nsc_srcs => "NSC_SRCS",
            .extra_srcs => "EXTRA_SRCS",
            .aux_srcs => "AUX_SRCS",
        };
    }

    /// Which of `cmake_parse_arguments()`'s three lists the keyword belongs
    /// to. An option takes no value at all, which is why `NO_NSC` cannot be
    /// treated as "a keyword whose values happen to be empty".
    pub const Group = enum { option, one_value, multi_value };

    pub fn group(self: Keyword) Group {
        return switch (self) {
            .no_nsc => .option,
            .name, .stack_bytes, .description, .board => .one_value,
            .uses, .libs, .off_target_libs, .nsc_srcs, .extra_srcs, .aux_srcs => .multi_value,
        };
    }
};

pub fn keywordOf(token: []const u8) ?Keyword {
    for (comptime std.enums.values(Keyword)) |candidate| {
        if (std.mem.eql(u8, token, candidate.spelling())) return candidate;
    }
    return null;
}

/// True for a token that LOOKS like a keyword: all caps, digits and
/// underscores, with at least one letter. A token that looks like a keyword
/// and is not one is refused rather than filed as a value, because that is
/// exactly what a typo'd or newly added keyword looks like.
pub fn keywordLike(token: []const u8) bool {
    if (token.len < 3) return false;
    var letters: usize = 0;
    for (token) |c| {
        if (std.ascii.isUpper(c)) {
            letters += 1;
        } else if (!std.ascii.isDigit(c) and c != '_') {
            return false;
        }
    }
    return letters > 0;
}

/// One kind of app: everything about a declaration that changes which arms of
/// `ra8_add_app()` it takes, and nothing that does not. `uses` is sorted, so
/// two apps that name the same middleware in a different order are the same
/// shape -- which they are, since the exports are unioned onto the app.
pub const Shape = struct {
    board: []const u8 = default_board,
    uses: []const []const u8 = &.{},
    no_nsc: bool = false,
    nsc_srcs: bool = false,
    extra_srcs: bool = false,
    aux_srcs: bool = false,
    off_target_libs: bool = false,

    pub fn eql(self: Shape, other: Shape) bool {
        if (!std.mem.eql(u8, self.board, other.board)) return false;
        if (self.uses.len != other.uses.len) return false;
        for (self.uses, other.uses) |mine, theirs| {
            if (!std.mem.eql(u8, mine, theirs)) return false;
        }
        return self.no_nsc == other.no_nsc and
            self.nsc_srcs == other.nsc_srcs and
            self.extra_srcs == other.extra_srcs and
            self.aux_srcs == other.aux_srcs and
            self.off_target_libs == other.off_target_libs;
    }

    /// `board=<b> uses=<a,b|-> flags=<a,b|->`, the spelling the ledger and the
    /// `shapes` step both use.
    pub fn label(self: Shape, allocator: std.mem.Allocator) ![]u8 {
        var out = std.ArrayList(u8).init(allocator);
        try out.writer().print("board={s} uses=", .{self.board});
        try writeList(out.writer(), self.uses);
        try out.appendSlice(" flags=");
        try self.writeFlags(out.writer());
        return out.toOwnedSlice();
    }

    pub fn writeFlags(self: Shape, writer: anytype) !void {
        const values = self.flagValues();
        var wrote = false;
        for (flag_names, 0..) |flag_name, i| {
            if (!values[i]) continue;
            if (wrote) try writer.writeByte(',');
            try writer.writeAll(flag_name);
            wrote = true;
        }
        if (!wrote) try writer.writeByte('-');
    }

    pub fn flagValues(self: Shape) [flag_names.len]bool {
        return .{ self.no_nsc, self.nsc_srcs, self.extra_srcs, self.aux_srcs, self.off_target_libs };
    }
};

/// The flag half of a shape, in the order `Shape.flagValues` returns it.
pub const flag_names = [_][]const u8{ "no_nsc", "nsc_srcs", "extra_srcs", "aux_srcs", "off_target_libs" };

/// One app that parsed.
pub const Row = struct {
    name: []const u8,
    listfile: []const u8,
    shape: Shape,
    stack_bytes: u32 = default_stack_bytes,
};

/// One declaration that did not parse, and why. A refusal is data, not a
/// warning to be lost down a pipe.
pub const Refusal = struct {
    listfile: []const u8,
    reason: []const u8,
};

pub const Entry = union(enum) {
    row: Row,
    refusal: Refusal,

    pub fn listfile(self: Entry) []const u8 {
        return switch (self) {
            .row => |r| r.listfile,
            .refusal => |r| r.listfile,
        };
    }

    pub fn name(self: Entry) []const u8 {
        return switch (self) {
            .row => |r| r.name,
            .refusal => "",
        };
    }
};

// ===========================================================================
// Reading the declarations out of a listfile
// ===========================================================================

pub const Token = struct { text: []const u8, quoted: bool };

/// CMake argument tokens: whitespace separated, `#` to end of line is a
/// comment, and a double-quoted run is one token however much whitespace it
/// holds (which is how `DESCRIPTION "two words"` survives).
pub const Tokenizer = struct {
    text: []const u8,
    index: usize = 0,

    pub fn next(self: *Tokenizer) ?Token {
        while (self.index < self.text.len) {
            const c = self.text[self.index];
            if (c == '#') {
                while (self.index < self.text.len and self.text[self.index] != '\n') self.index += 1;
                continue;
            }
            if (std.ascii.isWhitespace(c)) {
                self.index += 1;
                continue;
            }
            break;
        }
        if (self.index >= self.text.len) return null;
        if (self.text[self.index] == '"') {
            self.index += 1;
            const start = self.index;
            while (self.index < self.text.len and self.text[self.index] != '"') self.index += 1;
            const body = self.text[start..self.index];
            if (self.index < self.text.len) self.index += 1;
            return .{ .text = body, .quoted = true };
        }
        const start = self.index;
        while (self.index < self.text.len) {
            const c = self.text[self.index];
            if (std.ascii.isWhitespace(c) or c == '#') break;
            self.index += 1;
        }
        return .{ .text = self.text[start..self.index], .quoted = false };
    }
};

/// Walks the `ra8_add_app(...)` calls of one listfile, yielding each call's
/// argument text. Commented-out calls are skipped, and the closing paren is
/// found by depth so a call holding a `$<...>` generator expression or a
/// nested `if()` block still ends where it ends.
pub const BlockIterator = struct {
    text: []const u8,
    index: usize = 0,

    const call = "ra8_add_app";

    pub fn next(self: *BlockIterator) ?[]const u8 {
        while (std.mem.indexOfPos(u8, self.text, self.index, call)) |at| {
            self.index = at + call.len;
            if (at > 0 and isIdentChar(self.text[at - 1])) continue;
            var cursor = self.index;
            while (cursor < self.text.len and (self.text[cursor] == ' ' or self.text[cursor] == '\t')) cursor += 1;
            if (cursor >= self.text.len or self.text[cursor] != '(') continue;
            if (commentedOut(self.text, at)) continue;
            const body_start = cursor + 1;
            var depth: usize = 1;
            var scan = body_start;
            var in_quotes = false;
            while (scan < self.text.len) : (scan += 1) {
                const c = self.text[scan];
                if (in_quotes) {
                    if (c == '"') in_quotes = false;
                    continue;
                }
                switch (c) {
                    '"' => in_quotes = true,
                    '#' => while (scan < self.text.len and self.text[scan] != '\n') : (scan += 1) {},
                    '(' => depth += 1,
                    ')' => {
                        depth -= 1;
                        if (depth == 0) {
                            self.index = scan + 1;
                            return self.text[body_start..scan];
                        }
                    },
                    else => {},
                }
            }
            // An unterminated call: nothing left to read, and the caller sees
            // one fewer row rather than a wrong one. parseListfile turns this
            // into a refusal, which is where it belongs.
            self.index = self.text.len;
            return null;
        }
        return null;
    }

    fn isIdentChar(c: u8) bool {
        return std.ascii.isAlphanumeric(c) or c == '_';
    }

    fn commentedOut(text: []const u8, at: usize) bool {
        var back = at;
        while (back > 0) {
            back -= 1;
            if (text[back] == '\n') return false;
            if (text[back] == '#') return true;
        }
        return false;
    }
};

pub fn blocks(text: []const u8) BlockIterator {
    return .{ .text = text };
}

/// Reduce one call's argument text to an entry. Never fails on a malformed
/// declaration: it returns a refusal naming the listfile and the reason.
pub fn parseBlock(allocator: std.mem.Allocator, listfile: []const u8, body: []const u8) !Entry {
    var app_name: ?[]const u8 = null;
    var board: ?[]const u8 = null;
    var stack_text: ?[]const u8 = null;
    var uses = std.ArrayList([]const u8).init(allocator);
    var shape = Shape{};
    var current: ?Keyword = null;

    var tokens = Tokenizer{ .text = body };
    while (tokens.next()) |token| {
        if (token.text.len == 0) continue;
        if (!token.quoted) {
            if (keywordOf(token.text)) |keyword| {
                if (keyword == .no_nsc) {
                    shape.no_nsc = true;
                    current = null;
                } else {
                    current = keyword;
                }
                continue;
            }
            if (keywordLike(token.text)) {
                return .{ .refusal = .{
                    .listfile = listfile,
                    .reason = try std.fmt.allocPrint(
                        allocator,
                        "unrecognised keyword-like token {s}",
                        .{token.text},
                    ),
                } };
            }
        }
        const keyword = current orelse return .{ .refusal = .{
            .listfile = listfile,
            .reason = try std.fmt.allocPrint(
                allocator,
                "value {s} before any keyword",
                .{token.text},
            ),
        } };
        switch (keyword) {
            .name => app_name = app_name orelse token.text,
            .board => board = board orelse token.text,
            .stack_bytes => stack_text = stack_text orelse token.text,
            .uses => try uses.append(token.text),
            .nsc_srcs => shape.nsc_srcs = true,
            .extra_srcs => shape.extra_srcs = true,
            .aux_srcs => shape.aux_srcs = true,
            .off_target_libs => shape.off_target_libs = true,
            .description, .libs => {},
            .no_nsc => unreachable,
        }
    }

    const resolved_name = app_name orelse return .{ .refusal = .{
        .listfile = listfile,
        .reason = "declaration names no NAME",
    } };
    var stack = default_stack_bytes;
    if (stack_text) |text| {
        stack = std.fmt.parseInt(u32, text, 10) catch return .{ .refusal = .{
            .listfile = listfile,
            .reason = try std.fmt.allocPrint(allocator, "STACK_BYTES {s} is not a number", .{text}),
        } };
    }
    shape.board = board orelse default_board;
    shape.uses = try sortedCopy(allocator, uses.items);
    return .{ .row = .{
        .name = resolved_name,
        .listfile = listfile,
        .shape = shape,
        .stack_bytes = stack,
    } };
}

/// Every declaration in one listfile's text.
pub fn parseListfile(allocator: std.mem.Allocator, listfile: []const u8, text: []const u8) ![]Entry {
    var out = std.ArrayList(Entry).init(allocator);
    var iterator = blocks(text);
    while (iterator.next()) |body| try out.append(try parseBlock(allocator, listfile, body));
    return out.toOwnedSlice();
}

pub fn sortedCopy(allocator: std.mem.Allocator, items: []const []const u8) ![]const []const u8 {
    const copy = try allocator.alloc([]const u8, items.len);
    @memcpy(copy, items);
    std.mem.sort([]const u8, copy, {}, lessThanString);
    return copy;
}

fn lessThanString(_: void, a: []const u8, b: []const u8) bool {
    return std.mem.order(u8, a, b) == .lt;
}

pub fn sortEntries(entries: []Entry) void {
    std.mem.sort(Entry, entries, {}, lessThanEntry);
}

fn lessThanEntry(_: void, a: Entry, b: Entry) bool {
    const by_file = std.mem.order(u8, a.listfile(), b.listfile());
    if (by_file != .eq) return by_file == .lt;
    return std.mem.order(u8, a.name(), b.name()) == .lt;
}

// ===========================================================================
// The ledger
// ===========================================================================

/// TSV, one line per declaration. `app` rows carry the shape; `refusal` rows
/// carry the listfile and the reason. Both kinds are in one file on purpose:
/// a parser that starts refusing declarations changes the ledger instead of
/// quietly shrinking it.
pub fn renderLedger(allocator: std.mem.Allocator, entries: []const Entry) ![]u8 {
    var out = std.ArrayList(u8).init(allocator);
    const writer = out.writer();
    try writer.writeAll(
        \\# Generated by `zig build shapes`; do not hand-edit.
        \\# app<TAB>name<TAB>listfile<TAB>board<TAB>uses<TAB>flags<TAB>stack_bytes
        \\# refusal<TAB>listfile<TAB>reason
        \\
    );
    for (entries) |entry| switch (entry) {
        .row => |row| {
            try writer.print("app\t{s}\t{s}\t{s}\t", .{ row.name, row.listfile, row.shape.board });
            try writeList(writer, row.shape.uses);
            try writer.writeByte('\t');
            try row.shape.writeFlags(writer);
            try writer.print("\t{d}\n", .{row.stack_bytes});
        },
        .refusal => |refusal| try writer.print("refusal\t{s}\t{s}\n", .{ refusal.listfile, refusal.reason }),
    };
    return out.toOwnedSlice();
}

fn writeList(writer: anytype, items: []const []const u8) !void {
    if (items.len == 0) {
        try writer.writeByte('-');
        return;
    }
    for (items, 0..) |item, i| {
        if (i != 0) try writer.writeByte(',');
        try writer.writeAll(item);
    }
}

pub const LedgerError = error{
    UnknownKind,
    WrongFieldCount,
    UnknownFlag,
    BadStackBytes,
    EmptyLedger,
};

/// Read the ledger back. Refuses a line it does not understand rather than
/// skipping it, and refuses a ledger with no rows at all, so a test cannot
/// pass against an empty or truncated file.
pub fn parseLedger(allocator: std.mem.Allocator, text: []const u8) ![]Entry {
    var out = std.ArrayList(Entry).init(allocator);
    var lines = std.mem.splitScalar(u8, text, '\n');
    while (lines.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \r\t");
        if (line.len == 0 or line[0] == '#') continue;
        var fields = std.mem.splitScalar(u8, line, '\t');
        const kind = fields.next() orelse return LedgerError.WrongFieldCount;
        if (std.mem.eql(u8, kind, "refusal")) {
            const listfile = fields.next() orelse return LedgerError.WrongFieldCount;
            const reason = fields.next() orelse return LedgerError.WrongFieldCount;
            if (fields.next() != null) return LedgerError.WrongFieldCount;
            try out.append(.{ .refusal = .{ .listfile = listfile, .reason = reason } });
            continue;
        }
        if (!std.mem.eql(u8, kind, "app")) return LedgerError.UnknownKind;
        const app_name = fields.next() orelse return LedgerError.WrongFieldCount;
        const listfile = fields.next() orelse return LedgerError.WrongFieldCount;
        const board = fields.next() orelse return LedgerError.WrongFieldCount;
        const uses_field = fields.next() orelse return LedgerError.WrongFieldCount;
        const flags_field = fields.next() orelse return LedgerError.WrongFieldCount;
        const stack_field = fields.next() orelse return LedgerError.WrongFieldCount;
        if (fields.next() != null) return LedgerError.WrongFieldCount;
        var shape = Shape{ .board = board, .uses = try parseList(allocator, uses_field) };
        var flag_values = [_]bool{false} ** flag_names.len;
        if (!std.mem.eql(u8, flags_field, "-")) {
            var named = std.mem.splitScalar(u8, flags_field, ',');
            while (named.next()) |flag| {
                const index = flagIndex(flag) orelse return LedgerError.UnknownFlag;
                flag_values[index] = true;
            }
        }
        shape.no_nsc = flag_values[0];
        shape.nsc_srcs = flag_values[1];
        shape.extra_srcs = flag_values[2];
        shape.aux_srcs = flag_values[3];
        shape.off_target_libs = flag_values[4];
        const stack = std.fmt.parseInt(u32, stack_field, 10) catch return LedgerError.BadStackBytes;
        try out.append(.{ .row = .{
            .name = app_name,
            .listfile = listfile,
            .shape = shape,
            .stack_bytes = stack,
        } });
    }
    if (out.items.len == 0) return LedgerError.EmptyLedger;
    return out.toOwnedSlice();
}

fn parseList(allocator: std.mem.Allocator, field: []const u8) ![]const []const u8 {
    if (std.mem.eql(u8, field, "-")) return &.{};
    var out = std.ArrayList([]const u8).init(allocator);
    var parts = std.mem.splitScalar(u8, field, ',');
    while (parts.next()) |part| try out.append(part);
    return out.toOwnedSlice();
}

fn flagIndex(flag: []const u8) ?usize {
    for (flag_names, 0..) |candidate, i| {
        if (std.mem.eql(u8, flag, candidate)) return i;
    }
    return null;
}

/// The distinct shapes of a set of entries, in first-seen order.
pub fn distinctShapes(allocator: std.mem.Allocator, entries: []const Entry) ![]Shape {
    var out = std.ArrayList(Shape).init(allocator);
    for (entries) |entry| {
        const row = switch (entry) {
            .row => |r| r,
            .refusal => continue,
        };
        var seen = false;
        for (out.items) |shape| {
            if (shape.eql(row.shape)) {
                seen = true;
                break;
            }
        }
        if (!seen) try out.append(row.shape);
    }
    return out.toOwnedSlice();
}

pub fn countRows(entries: []const Entry) usize {
    var total: usize = 0;
    for (entries) |entry| if (entry == .row) {
        total += 1;
    };
    return total;
}

pub fn countRefusals(entries: []const Entry) usize {
    var total: usize = 0;
    for (entries) |entry| if (entry == .refusal) {
        total += 1;
    };
    return total;
}

pub fn rowsWithShape(entries: []const Entry, shape: Shape) usize {
    var total: usize = 0;
    for (entries) |entry| switch (entry) {
        .row => |row| if (row.shape.eql(shape)) {
            total += 1;
        },
        .refusal => {},
    };
    return total;
}

// ===========================================================================
// What app_table.zig does NOT cross-build yet
// ===========================================================================

/// One kind of app the cross-build table has never built, with the app that
/// would be the cheapest one to add and what adding it would cost. Held in
/// BOTH directions by app_shapes_test.zig: a shape in the ledger that is
/// neither cross-built nor listed here fails, and an entry here that no longer
/// matches any declaration fails too, so this table cannot rot into a list of
/// shapes the tree stopped having.
pub const Uncovered = struct {
    representative: []const u8,
    shape: Shape,
    note: []const u8,
};

pub const uncovered = [_]Uncovered{
    .{
        .representative = "usb_selftest_wlun",
        .shape = .{ .uses = &.{ "threadx", "usbx" } },
        .note = "25 declarations, the largest uncovered kind by a long way: USBX device classes on top of ThreadX. middleware.zig already compiles ThreadX; USBX is the next middleware to teach it",
    },
    .{
        .representative = "c6_mdl_test",
        .shape = .{ .uses = &.{ "esp_hosted", "threadx" } },
        .note = "4 declarations: the C6 co-processor host stack, which brings its own generated sources",
    },
    .{
        .representative = "threadx_fs_demo",
        .shape = .{ .uses = &.{ "levelx", "threadx" } },
        .note = "3 declarations: LevelX over ThreadX, the flash-translation stack",
    },
    .{
        .representative = "threadx_https_client",
        .shape = .{ .uses = &.{ "mbedtls", "netxduo", "threadx" } },
        .note = "2 declarations: the TLS stack, three middlewares deep",
    },
    .{
        .representative = "c6_wifi_join",
        .shape = .{ .uses = &.{ "esp_hosted", "netxduo", "threadx" }, .extra_srcs = true },
        .note = "2 declarations: C6 Wi-Fi with NetX Duo, and the first uncovered kind that also compiles EXTRA_SRCS",
    },
    .{
        .representative = "threadx_nimble_peripheral",
        .shape = .{ .uses = &.{ "nimble", "threadx" } },
        .note = "1 declaration: the NimBLE host",
    },
    .{
        .representative = "ra8_cache_store_demo",
        .shape = .{ .uses = &.{"levelx_standalone"} },
        .note = "1 declaration, and the only uncovered kind with NO ThreadX at all: LevelX in standalone mode (#616)",
    },
    .{
        .representative = "threadx_netx_tcp_echo",
        .shape = .{ .uses = &.{ "netxduo", "threadx" } },
        .note = "1 declaration: NetX Duo without TLS",
    },
    .{
        .representative = "npu_infer",
        .shape = .{ .uses = &.{"tflite_micro"} },
        .note = "1 declaration: the only C++ middleware in the tree",
    },
    .{
        .representative = "dfu_bootloader",
        .shape = .{ .uses = &.{ "threadx", "usbx" }, .extra_srcs = true },
        .note = "1 declaration: USBX plus EXTRA_SRCS, so it is not covered by usb_selftest_wlun's shape",
    },
    .{
        .representative = "media_download",
        .shape = .{ .uses = &.{ "esp_hosted", "threadx" }, .extra_srcs = true },
        .note = "1 declaration: C6 host plus EXTRA_SRCS",
    },
    .{
        .representative = "secure_boot_ns_hil",
        .shape = .{ .extra_srcs = true, .aux_srcs = true },
        .note = "1 declaration, and the cheapest one to add: no middleware at all, just EXTRA_SRCS and AUX_SRCS together, which secure_boot_hil and cpu1_pingpong each take one half of",
    },
};

pub fn uncoveredFor(shape: Shape) ?Uncovered {
    for (uncovered) |candidate| {
        if (candidate.shape.eql(shape)) return candidate;
    }
    return null;
}

// ===========================================================================
// The keyword set, read from the listfile that declares it
// ===========================================================================

pub const CmakeKeywords = struct {
    options: []const []const u8,
    one_value: []const []const u8,
    multi_value: []const []const u8,
};

/// Extract the three keyword lists `cmake/ra8_add_app.cmake` hands
/// `cmake_parse_arguments()`: the first three double-quoted arguments of the
/// first call in the file, each a `;`-separated list. Returns null when the
/// call is not there in the shape this expects, which is a failing test rather
/// than a guess.
pub fn keywordsFromCmake(allocator: std.mem.Allocator, text: []const u8) !?CmakeKeywords {
    const at = std.mem.indexOf(u8, text, "cmake_parse_arguments") orelse return null;
    var cursor = at;
    var lists: [3][]const []const u8 = undefined;
    var found: usize = 0;
    while (found < lists.len) {
        const open = std.mem.indexOfScalarPos(u8, text, cursor, '"') orelse return null;
        const close = std.mem.indexOfScalarPos(u8, text, open + 1, '"') orelse return null;
        lists[found] = try splitSemicolons(allocator, text[open + 1 .. close]);
        found += 1;
        cursor = close + 1;
    }
    return .{ .options = lists[0], .one_value = lists[1], .multi_value = lists[2] };
}

fn splitSemicolons(allocator: std.mem.Allocator, field: []const u8) ![]const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    var parts = std.mem.splitScalar(u8, field, ';');
    while (parts.next()) |part| {
        const trimmed = std.mem.trim(u8, part, " \t\r\n");
        if (trimmed.len != 0) try out.append(trimmed);
    }
    return out.toOwnedSlice();
}

// ===========================================================================
// The build step, and the parity row it reports (#1322)
// ===========================================================================

/// Ceiling on one listfile read for the ledger. The largest app listfile in
/// the tree is under 8 KiB; this is generous and finite rather than unbounded.
const max_listfile_bytes = 256 * 1024;

/// What the ledger says about the tree, for the parity manifest: how many
/// declarations it carries, how many distinct kinds of app that is, how many
/// of those kinds no app in the cross-build table has ever been built as, and
/// how many declarations this parser refused to read.
pub const Summary = struct {
    rows: usize,
    kinds: usize,
    uncovered: usize,
    refusals: usize,
};

/// Every `ra8_add_app()` declaration under `roots`, parsed into entries and
/// sorted by listfile. Read at configure time from the build root's own
/// listfiles: a declaration this cannot read becomes a refusal row in the
/// ledger rather than a missing app, which is what makes the committed ledger
/// a check and not a summary.
pub fn collect(b: *std.Build) []Entry {
    var entries = std.ArrayList(Entry).init(b.allocator);
    for (roots) |tree| {
        var dir = b.build_root.handle.openDir(tree, .{ .iterate = true }) catch |err| std.debug.panic(
            "ra8: cannot read {s}/ for the app-shape ledger: {s}",
            .{ tree, @errorName(err) },
        );
        defer dir.close();
        var walker = dir.walk(b.allocator) catch @panic("OOM");
        defer walker.deinit();
        while (walker.next() catch |err| std.debug.panic(
            "ra8: cannot walk {s}/ for the app-shape ledger: {s}",
            .{ tree, @errorName(err) },
        )) |entry| {
            if (entry.kind != .file) continue;
            if (!std.mem.eql(u8, std.fs.path.basename(entry.path), "CMakeLists.txt")) continue;
            const listfile = b.fmt("{s}/{s}", .{ tree, entry.path });
            const text = b.build_root.handle.readFileAlloc(
                b.allocator,
                listfile,
                max_listfile_bytes,
            ) catch |err| std.debug.panic(
                "ra8: cannot read {s} for the app-shape ledger: {s}",
                .{ listfile, @errorName(err) },
            );
            const parsed = parseListfile(b.allocator, listfile, text) catch @panic("OOM");
            entries.appendSlice(parsed) catch @panic("OOM");
        }
    }
    const owned = entries.toOwnedSlice() catch @panic("OOM");
    sortEntries(owned);
    return owned;
}

/// What `zig build shapes` says it does. The step itself is declared in
/// build.zig, which is where the command-surface gate reads the graph's steps.
pub const step_description = "Hold the committed app-shape ledger to the tree's own ra8_add_app() declarations";

/// Wire `zig build shapes`: regenerate the ledger from the tree, install the
/// regenerated copy, and diff it against the committed file. `test_step`
/// gains it, so an ordinary test run fails on a tree the committed ledger no
/// longer describes.
pub fn add(b: *std.Build, step: *std.Build.Step, test_step: *std.Build.Step) Summary {
    const entries = collect(b);
    const text = renderLedger(b.allocator, entries) catch @panic("OOM");
    const files = b.addWriteFiles();
    const generated = files.add("app_shape_ledger.tsv", text);
    const install = b.addInstallFileWithDir(generated, .prefix, "app_shape_ledger.tsv");
    step.dependOn(&install.step);
    const ledger_diff = b.addSystemCommand(&.{ "diff", "-u" });
    ledger_diff.addFileArg(b.path(ledger_path));
    ledger_diff.addFileArg(generated);
    ledger_diff.step.dependOn(&install.step);
    step.dependOn(&ledger_diff.step);
    test_step.dependOn(step);

    const kinds = distinctShapes(b.allocator, entries) catch @panic("OOM");
    var not_cross_built: usize = 0;
    for (kinds) |shape| {
        if (uncoveredFor(shape) != null) not_cross_built += 1;
    }
    return .{
        .rows = countRows(entries),
        .kinds = kinds.len,
        .uncovered = not_cross_built,
        .refusals = countRefusals(entries),
    };
}

/// The app tree's row on `zig build parity`, in the manifest's three-column
/// shape: the slice, the file that carries it, and what it measures.
pub fn addParityRow(b: *std.Build, parity_step: *std.Build.Step, summary: Summary) void {
    const print = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
    print.addArg("app_shapes");
    print.addArg(ledger_path);
    print.addArg(b.fmt("{d} declarations, {d} kinds, {d} not cross-built, {d} refused", .{
        summary.rows,
        summary.kinds,
        summary.uncovered,
        summary.refusals,
    }));
    parity_step.dependOn(&print.step);
}
