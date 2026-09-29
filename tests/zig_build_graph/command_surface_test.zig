//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The command-surface parity rules of command_surface.zig, applied to the
//! REAL build.zig and the REAL just/zig.just (#1165, part of #857).
//!
//! Both files arrive as anonymous imports declared in build.zig, so they are
//! read at COMPILE time from the paths the build graph itself names. A test
//! that opened them through std.fs would be asserting something about the
//! working directory it happened to run in, and would pass vacuously wherever
//! that guess was wrong.
//!
//! In its own file rather than in command_surface.zig because build.zig
//! imports that module directly: an @embedFile of an import name only the
//! test module declares cannot compile in the build runner.

const std = @import("std");
const graph = @import("build_graph");
const command_surface = graph.command_surface;

const build_zig_source = @embedFile("build_zig_source");
const just_zig_source = @embedFile("just_zig_source");

/// The recipes that legitimately dispatch to no step: the help menu, the
/// fail-closed toolchain probe, and the one recipe that removes this graph's
/// own outputs without invoking it. Named here so a NEW non-dispatching
/// recipe has to be considered rather than quietly joining them.
const non_dispatching = [_][]const u8{ "default", "_zig", "clean" };

test "every step build.zig declares has a just recipe, and every recipe a step" {
    const allocator = std.testing.allocator;

    const steps = try command_surface.declaredSteps(allocator, build_zig_source);
    defer allocator.free(steps);
    const recipes = try command_surface.parseRecipes(allocator, just_zig_source);
    defer allocator.free(recipes);

    // Refuse to report clean against nothing: both parsers stopping would
    // otherwise satisfy every rule below.
    try std.testing.expect(steps.len >= 9);
    try std.testing.expect(recipes.len >= 11);

    const missing = try command_surface.stepsWithoutRecipe(allocator, steps, recipes);
    defer allocator.free(missing);
    if (missing.len != 0) {
        std.debug.print("steps with no `just zig::` recipe:", .{});
        for (missing) |step| std.debug.print(" {s}", .{step});
        std.debug.print("\n", .{});
    }
    try std.testing.expectEqual(@as(usize, 0), missing.len);

    const unknown = try command_surface.recipesWithUnknownStep(allocator, steps, recipes);
    defer allocator.free(unknown);
    if (unknown.len != 0) {
        std.debug.print("recipes dispatching to an undeclared step:", .{});
        for (unknown) |recipe| std.debug.print(" {s}", .{recipe});
        std.debug.print("\n", .{});
    }
    try std.testing.expectEqual(@as(usize, 0), unknown.len);
}

test "every public recipe is in the help menu, and every menu line is a recipe" {
    const allocator = std.testing.allocator;

    const recipes = try command_surface.parseRecipes(allocator, just_zig_source);
    defer allocator.free(recipes);
    const entries = try command_surface.helpMenuEntries(allocator, just_zig_source);
    defer allocator.free(entries);
    try std.testing.expect(entries.len >= 11);

    const undocumented = try command_surface.recipesMissingHelp(allocator, recipes, entries);
    defer allocator.free(undocumented);
    if (undocumented.len != 0) {
        std.debug.print("recipes missing from the help menu:", .{});
        for (undocumented) |recipe| std.debug.print(" {s}", .{recipe});
        std.debug.print("\n", .{});
    }
    try std.testing.expectEqual(@as(usize, 0), undocumented.len);

    const stale = try command_surface.helpEntriesWithoutRecipe(allocator, recipes, entries);
    defer allocator.free(stale);
    if (stale.len != 0) {
        std.debug.print("help-menu lines naming no recipe:", .{});
        for (stale) |entry| std.debug.print(" {s}", .{entry});
        std.debug.print("\n", .{});
    }
    try std.testing.expectEqual(@as(usize, 0), stale.len);
}

test "no recipe passes anything to zig build but a step name" {
    const allocator = std.testing.allocator;

    const recipes = try command_surface.parseRecipes(allocator, just_zig_source);
    defer allocator.free(recipes);

    const offenders = try command_surface.recipesWithExtraArgs(allocator, recipes);
    defer allocator.free(offenders);
    if (offenders.len != 0) {
        std.debug.print("recipes assembling arguments for zig build:", .{});
        for (offenders) |recipe| std.debug.print(" {s}", .{recipe});
        std.debug.print("\n", .{});
    }
    try std.testing.expectEqual(@as(usize, 0), offenders.len);
}

test "the two steps #1165 found unreachable are both on the surface now" {
    const allocator = std.testing.allocator;

    const recipes = try command_surface.parseRecipes(allocator, just_zig_source);
    defer allocator.free(recipes);

    // analysis (#1157) and abi (#1007) are the two that were declared and
    // unreachable. Named directly so a slice that drops either recipe fails
    // here as well as in the mapping test above.
    for ([_][]const u8{ "analysis", "abi" }) |step| {
        var found = false;
        for (recipes) |recipe| {
            const dispatch = recipe.dispatch orelse continue;
            if (std.mem.eql(u8, dispatch, step)) {
                found = true;
                try std.testing.expectEqualStrings(step, recipe.name);
            }
        }
        try std.testing.expect(found);
    }
}

test "only the three known recipes invoke no build step" {
    const allocator = std.testing.allocator;

    const recipes = try command_surface.parseRecipes(allocator, just_zig_source);
    defer allocator.free(recipes);

    for (recipes) |recipe| {
        if (recipe.dispatch != null) continue;
        var expected = false;
        for (non_dispatching) |name| {
            if (std.mem.eql(u8, recipe.name, name)) expected = true;
        }
        if (!expected) std.debug.print("recipe `{s}` invokes no build step\n", .{recipe.name});
        try std.testing.expect(expected);
    }
}
