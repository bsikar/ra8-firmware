//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The command surface of the root Zig build graph: the steps `build.zig`
//! declares, the `just/zig.just` recipes that dispatch to them, and the help
//! menu that advertises those recipes (#1165, part of #857).
//!
//! Three hand-maintained lists of the same thing, and until this module
//! nothing compared them. A slice that adds a step and forgets the recipe
//! costs nothing at the time: the step works, `zig build test` still depends
//! on it, the graph stays green, and the step is simply unreachable from the
//! surface a developer and `just ci` use. `analysis` (#1157) and `abi`
//! (#1007) were both in that state.
//!
//! Everything here is a pure function over source TEXT, so the rules are unit
//! tested on fixtures in this file and then applied to the REAL two files in
//! command_surface_test.zig, which embeds them. Nothing reads the filesystem
//! or assumes a working directory.

const std = @import("std");

/// Hand this graph's own two command-surface sources to a test module as
/// compile-time imports: `build.zig`, which declares the steps, and
/// `just/zig.just`, which exposes them.
///
/// Anonymous imports rather than a std.fs read in the test, because the paths
/// are resolved by the build graph that owns them; a test opening them itself
/// would be asserting something about the working directory it ran in.
pub fn addSources(b: *std.Build, module: *std.Build.Module) void {
    module.addAnonymousImport("build_zig_source", .{
        .root_source_file = b.path("build.zig"),
    });
    module.addAnonymousImport("just_zig_source", .{
        .root_source_file = b.path("just/zig.just"),
    });
}

/// A `just` recipe of just/zig.just, reduced to what the parity rules need.
pub const Recipe = struct {
    /// Recipe name as `just zig::<name>` spells it.
    name: []const u8,
    /// The step this recipe passes to `zig build`, empty for the default
    /// install step (`just zig::build`), null when the recipe never invokes
    /// the Zig toolchain at all (`just zig::clean`, the help menu).
    dispatch: ?[]const u8 = null,
    /// Arguments beyond a single step name on the `zig build` line. A command
    /// layer that starts assembling compiler arguments has re-created the
    /// problem this migration exists to retire, so it is a parity failure.
    extra_args: bool = false,

    /// Report whether this recipe is private to the justfile. `just`'s own
    /// convention: a leading underscore hides it from `--list`, so it owes no
    /// help-menu line (`_zig`, the fail-closed toolchain probe).
    pub fn isPrivate(self: Recipe) bool {
        return self.name.len > 0 and self.name[0] == '_';
    }
};

/// Parse the step names `build.zig` declares, in declaration order.
///
/// Reads the first argument of every `b.step(` call. A call whose first
/// argument is not a string literal is an error rather than a skip: a step
/// this parser cannot see is a step the parity rules would silently excuse.
pub fn declaredSteps(allocator: std.mem.Allocator, build_zig: []const u8) ![][]const u8 {
    var names = std.ArrayList([]const u8).init(allocator);
    errdefer names.deinit();

    const needle = "b.step(";
    var cursor: usize = 0;
    while (std.mem.indexOfPos(u8, build_zig, cursor, needle)) |hit| {
        var index = hit + needle.len;
        while (index < build_zig.len and std.ascii.isWhitespace(build_zig[index])) index += 1;
        if (index >= build_zig.len or build_zig[index] != '"') return error.UnparsableStepName;
        index += 1;
        const start = index;
        while (index < build_zig.len and build_zig[index] != '"') index += 1;
        if (index >= build_zig.len) return error.UnparsableStepName;
        try names.append(build_zig[start..index]);
        cursor = index + 1;
    }
    return names.toOwnedSlice();
}

/// Parse the recipes of a justfile module, in declaration order.
///
/// A recipe header is a column-0 `name:` line. `set`, `export` and `mod`
/// lines are assignments and imports, not recipes, and `:=` is what tells
/// them apart from a recipe whose name happens to end the same way.
pub fn parseRecipes(allocator: std.mem.Allocator, just_source: []const u8) ![]Recipe {
    var recipes = std.ArrayList(Recipe).init(allocator);
    errdefer recipes.deinit();

    var lines = std.mem.splitScalar(u8, just_source, '\n');
    while (lines.next()) |line| {
        if (line.len == 0) continue;
        if (std.ascii.isWhitespace(line[0])) {
            // A body line. Attribute a `zig build` dispatch to the recipe it
            // sits under; a body line before any header cannot exist in a
            // justfile `just` itself accepts.
            if (recipes.items.len == 0) continue;
            const recipe = &recipes.items[recipes.items.len - 1];
            if (std.mem.indexOf(u8, line, "${RA8_ZIG}") == null) continue;
            var tokens = std.mem.tokenizeAny(u8, line, " \t");
            _ = tokens.next() orelse continue; // the quoted toolchain
            const verb = tokens.next() orelse continue;
            if (!std.mem.eql(u8, verb, "build")) continue;
            recipe.dispatch = tokens.next() orelse "";
            recipe.extra_args = tokens.next() != null;
            continue;
        }
        if (line[0] == '#') continue;
        const name = leadingName(line);
        if (name.len == 0) continue;
        if (name.len + 1 > line.len or line[name.len] != ':') continue;
        if (name.len + 1 < line.len and line[name.len + 1] == '=') continue;
        try recipes.append(.{ .name = name });
    }
    return recipes.toOwnedSlice();
}

/// The recipe names the `default` help menu advertises, in printed order.
///
/// Every menu line spells the fully qualified command, so `just zig::` is the
/// anchor and what follows it is the recipe name.
pub fn helpMenuEntries(allocator: std.mem.Allocator, just_source: []const u8) ![][]const u8 {
    var entries = std.ArrayList([]const u8).init(allocator);
    errdefer entries.deinit();

    const needle = "just zig::";
    var cursor: usize = 0;
    while (std.mem.indexOfPos(u8, just_source, cursor, needle)) |hit| {
        var index = hit + needle.len;
        const start = index;
        while (index < just_source.len and isNameByte(just_source[index])) index += 1;
        if (index > start) try entries.append(just_source[start..index]);
        cursor = if (index > start) index else hit + needle.len;
    }
    return entries.toOwnedSlice();
}

/// Declared steps no recipe dispatches to: a step reachable only by typing
/// `zig build <step>` by hand.
pub fn stepsWithoutRecipe(
    allocator: std.mem.Allocator,
    steps: []const []const u8,
    recipes: []const Recipe,
) ![][]const u8 {
    var missing = std.ArrayList([]const u8).init(allocator);
    errdefer missing.deinit();
    for (steps) |step| {
        var covered = false;
        for (recipes) |recipe| {
            const dispatch = recipe.dispatch orelse continue;
            if (std.mem.eql(u8, dispatch, step)) covered = true;
        }
        if (!covered) try missing.append(step);
    }
    return missing.toOwnedSlice();
}

/// Recipes that dispatch to a step `build.zig` does not declare: a recipe
/// that fails at the moment someone runs it.
pub fn recipesWithUnknownStep(
    allocator: std.mem.Allocator,
    steps: []const []const u8,
    recipes: []const Recipe,
) ![][]const u8 {
    var unknown = std.ArrayList([]const u8).init(allocator);
    errdefer unknown.deinit();
    for (recipes) |recipe| {
        const dispatch = recipe.dispatch orelse continue;
        if (dispatch.len == 0) continue; // the default install step
        var declared = false;
        for (steps) |step| {
            if (std.mem.eql(u8, dispatch, step)) declared = true;
        }
        if (!declared) try unknown.append(recipe.name);
    }
    return unknown.toOwnedSlice();
}

/// Recipes passing anything beyond a step name to `zig build`.
pub fn recipesWithExtraArgs(
    allocator: std.mem.Allocator,
    recipes: []const Recipe,
) ![][]const u8 {
    var offenders = std.ArrayList([]const u8).init(allocator);
    errdefer offenders.deinit();
    for (recipes) |recipe| {
        if (recipe.extra_args) try offenders.append(recipe.name);
    }
    return offenders.toOwnedSlice();
}

/// Public recipes the help menu never mentions. `default` is the menu itself
/// and a private recipe is hidden from `just --list`, so neither owes a line.
pub fn recipesMissingHelp(
    allocator: std.mem.Allocator,
    recipes: []const Recipe,
    entries: []const []const u8,
) ![][]const u8 {
    var missing = std.ArrayList([]const u8).init(allocator);
    errdefer missing.deinit();
    for (recipes) |recipe| {
        if (recipe.isPrivate()) continue;
        if (std.mem.eql(u8, recipe.name, "default")) continue;
        var listed = false;
        for (entries) |entry| {
            if (std.mem.eql(u8, entry, recipe.name)) listed = true;
        }
        if (!listed) try missing.append(recipe.name);
    }
    return missing.toOwnedSlice();
}

/// Help-menu lines naming a recipe that does not exist: the other drift
/// direction, a command the menu promises and `just` rejects.
pub fn helpEntriesWithoutRecipe(
    allocator: std.mem.Allocator,
    recipes: []const Recipe,
    entries: []const []const u8,
) ![][]const u8 {
    var stale = std.ArrayList([]const u8).init(allocator);
    errdefer stale.deinit();
    for (entries) |entry| {
        var exists = false;
        for (recipes) |recipe| {
            if (std.mem.eql(u8, recipe.name, entry)) exists = true;
        }
        if (!exists) try stale.append(entry);
    }
    return stale.toOwnedSlice();
}

fn leadingName(line: []const u8) []const u8 {
    var index: usize = 0;
    while (index < line.len and isNameByte(line[index])) index += 1;
    return line[0..index];
}

fn isNameByte(byte: u8) bool {
    return std.ascii.isAlphanumeric(byte) or byte == '_' or byte == '-';
}

const fixture_just =
    \\set working-directory := ".."
    \\
    \\export RA8_ZIG := env('RA8_ZIG', 'zig')
    \\
    \\# Show the help menu
    \\default:
    \\    @echo "  just zig::build             Build the archives"
    \\    @echo "  just zig::test              Run every suite"
    \\    @echo "  just zig::clean             Remove the outputs"
    \\
    \\_zig:
    \\    #!/usr/bin/env bash
    \\    command -v "${RA8_ZIG}"
    \\
    \\build: _zig
    \\    "${RA8_ZIG}" build
    \\
    \\test: _zig
    \\    "${RA8_ZIG}" build test
    \\
    \\clean:
    \\    rm -rf zig-out .zig-cache
    \\
;

test "the recipe parser reads dispatch, privacy and assignments apart" {
    const allocator = std.testing.allocator;
    const recipes = try parseRecipes(allocator, fixture_just);
    defer allocator.free(recipes);

    try std.testing.expectEqual(@as(usize, 5), recipes.len);
    try std.testing.expectEqualStrings("default", recipes[0].name);
    try std.testing.expectEqual(@as(?[]const u8, null), recipes[0].dispatch);
    try std.testing.expectEqualStrings("_zig", recipes[1].name);
    try std.testing.expect(recipes[1].isPrivate());
    // `command -v "${RA8_ZIG}"` names the toolchain without building.
    try std.testing.expectEqual(@as(?[]const u8, null), recipes[1].dispatch);
    try std.testing.expectEqualStrings("build", recipes[2].name);
    try std.testing.expectEqualStrings("", recipes[2].dispatch.?);
    try std.testing.expectEqualStrings("test", recipes[3].name);
    try std.testing.expectEqualStrings("test", recipes[3].dispatch.?);
    try std.testing.expectEqualStrings("clean", recipes[4].name);
    try std.testing.expectEqual(@as(?[]const u8, null), recipes[4].dispatch);
    for (recipes) |recipe| try std.testing.expect(!recipe.extra_args);
}

test "the step parser reads every b.step spelling this graph uses" {
    const allocator = std.testing.allocator;
    const source =
        \\    const test_step = b.step("test", "Run it");
        \\    const arm_step = b.step("arm", b.fmt("Cross-build {d} apps", .{n}));
        \\    const soup_step = b.step(
        \\        "test-soup",
        \\        "Compile the vendored C",
        \\    );
        \\
    ;
    const steps = try declaredSteps(allocator, source);
    defer allocator.free(steps);
    try std.testing.expectEqual(@as(usize, 3), steps.len);
    try std.testing.expectEqualStrings("test", steps[0]);
    try std.testing.expectEqualStrings("arm", steps[1]);
    try std.testing.expectEqualStrings("test-soup", steps[2]);
}

test "a step with no recipe is the drift #1165 found, in both directions" {
    const allocator = std.testing.allocator;
    const recipes = try parseRecipes(allocator, fixture_just);
    defer allocator.free(recipes);

    // The state this slice closes: the graph declares a step the command
    // surface never exposes.
    const declared = [_][]const u8{ "test", "analysis" };
    const missing = try stepsWithoutRecipe(allocator, &declared, recipes);
    defer allocator.free(missing);
    try std.testing.expectEqual(@as(usize, 1), missing.len);
    try std.testing.expectEqualStrings("analysis", missing[0]);

    // The opposite drift: a recipe left behind by a renamed step.
    const renamed = [_][]const u8{"test-all"};
    const unknown = try recipesWithUnknownStep(allocator, &renamed, recipes);
    defer allocator.free(unknown);
    try std.testing.expectEqual(@as(usize, 1), unknown.len);
    try std.testing.expectEqualStrings("test", unknown[0]);

    // `just zig::build` dispatches to the default install step, which no
    // b.step() call declares, and must not read as unknown.
    const only_test = [_][]const u8{"test"};
    const clean = try recipesWithUnknownStep(allocator, &only_test, recipes);
    defer allocator.free(clean);
    try std.testing.expectEqual(@as(usize, 0), clean.len);
}

test "the help menu is held to the recipe list in both directions" {
    const allocator = std.testing.allocator;
    const recipes = try parseRecipes(allocator, fixture_just);
    defer allocator.free(recipes);
    const entries = try helpMenuEntries(allocator, fixture_just);
    defer allocator.free(entries);

    try std.testing.expectEqual(@as(usize, 3), entries.len);
    const listed = try recipesMissingHelp(allocator, recipes, entries);
    defer allocator.free(listed);
    try std.testing.expectEqual(@as(usize, 0), listed.len);
    const stale = try helpEntriesWithoutRecipe(allocator, recipes, entries);
    defer allocator.free(stale);
    try std.testing.expectEqual(@as(usize, 0), stale.len);

    // Drop the menu and every public recipe is undocumented; `default` and
    // the private probe still owe nothing.
    const none: []const []const u8 = &.{};
    const undocumented = try recipesMissingHelp(allocator, recipes, none);
    defer allocator.free(undocumented);
    try std.testing.expectEqual(@as(usize, 3), undocumented.len);
    try std.testing.expectEqualStrings("build", undocumented[0]);
    try std.testing.expectEqualStrings("test", undocumented[1]);
    try std.testing.expectEqualStrings("clean", undocumented[2]);

    // A menu line for a command that does not exist.
    const promised = [_][]const u8{"analysis"};
    const missing = try helpEntriesWithoutRecipe(allocator, recipes, &promised);
    defer allocator.free(missing);
    try std.testing.expectEqual(@as(usize, 1), missing.len);
    try std.testing.expectEqualStrings("analysis", missing[0]);
}

test "a recipe that assembles compiler arguments is a parity failure" {
    const allocator = std.testing.allocator;
    const source =
        \\arm: _zig
        \\    "${RA8_ZIG}" build arm -Dtarget=thumb-freestanding -Doptimize=ReleaseSmall
        \\
    ;
    const recipes = try parseRecipes(allocator, source);
    defer allocator.free(recipes);
    try std.testing.expectEqualStrings("arm", recipes[0].dispatch.?);
    try std.testing.expect(recipes[0].extra_args);

    const offenders = try recipesWithExtraArgs(allocator, recipes);
    defer allocator.free(offenders);
    try std.testing.expectEqual(@as(usize, 1), offenders.len);
    try std.testing.expectEqualStrings("arm", offenders[0]);
}
