//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the decidable half of
//! `check_host_build_entrypoints` (#1335, part of #858). Every case here pins a
//! behaviour of the deleted Python, including the regex quirks that a tidier
//! matcher would silently widen: the anchored RECIPE match, the backtracking
//! `\s+` in RAW_CMAKE_CONFIGURE, and the separator RAW_COMPILER eats before its
//! trailing alternation.

const std = @import("std");
const impl = @import("implementation");
const testing = std.testing;

fn collectLines(allocator: std.mem.Allocator, text: []const u8) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer out.deinit();
    var it = impl.LineIterator{ .text = text };
    while (it.next()) |line| try out.append(line);
    return out.toOwnedSlice();
}

fn bodyOf(recipes: []impl.Recipe, name: []const u8) ?[]const u8 {
    for (recipes) |recipe| {
        if (std.mem.eql(u8, recipe.name, name)) return recipe.body;
    }
    return null;
}

fn errorCount(label: []const u8, text: []const u8) !usize {
    const errors = try impl.recipeErrors(testing.allocator, label, text);
    defer impl.freeStrings(testing.allocator, errors);
    return errors.len;
}

fn firstError(allocator: std.mem.Allocator, label: []const u8, text: []const u8) !?[]const u8 {
    const errors = try impl.recipeErrors(allocator, label, text);
    defer impl.freeStrings(allocator, errors);
    if (errors.len == 0) return null;
    return try allocator.dupe(u8, errors[0]);
}

fn commandCount(body: []const u8) !usize {
    const commands = try impl.shellCommands(testing.allocator, body);
    defer impl.freeStrings(testing.allocator, commands);
    return commands.len;
}

// -- the inherited contract literals -----------------------------------------

test "tool name is the migrated tool, not the deleted module" {
    try testing.expectEqualStrings("check_host_build_entrypoints", impl.tool);
}

test "compiled suffixes are the six the predecessor counted" {
    try testing.expectEqual(@as(usize, 6), impl.compiled_suffixes.len);
    try testing.expectEqualStrings(".c", impl.compiled_suffixes[0]);
    try testing.expectEqualStrings(".mm", impl.compiled_suffixes[5]);
}

test "shared Just delegation keeps the literal Just interpolation" {
    try testing.expectEqualStrings("build_shared_libs.sh \"{{ lib }}\"", impl.shared_just_delegation);
}

test "shared dispatcher contract names the standalone probe and the wrapper" {
    try testing.expect(impl.containsString(&impl.shared_dispatcher_contract, "is_standalone"));
    try testing.expect(impl.containsString(&impl.shared_dispatcher_contract, "host_cmake.sh"));
    try testing.expect(impl.containsString(&impl.shared_dispatcher_contract, "apps::shared::test"));
}

test "tools Just contract carries both recipes and both dispatch lines" {
    try testing.expectEqual(@as(usize, 4), impl.tools_just_contract.len);
    try testing.expect(impl.containsString(&impl.tools_just_contract, "build tool=\"all\":"));
    try testing.expect(impl.containsString(&impl.tools_just_contract, "build_host_tools.sh clean \"{{ tool }}\""));
}

test "host cmake contract pins compiler selection and cache reset" {
    try testing.expectEqual(@as(usize, 5), impl.host_cmake_contract.len);
    try testing.expect(impl.containsString(&impl.host_cmake_contract, "ra8_select_host_compiler"));
    try testing.expect(impl.containsString(&impl.host_cmake_contract, "ra8_cmake_reset_if_incompatible"));
}

test "dispatcher clean contract still names the legacy artefacts" {
    try testing.expectEqual(@as(usize, 4), impl.dispatcher_clean_contract.len);
    try testing.expect(impl.containsString(&impl.dispatcher_clean_contract, "clean_one"));
    try testing.expect(impl.containsString(&impl.dispatcher_clean_contract, "\"$dir\"/*.trace"));
}

// -- code point decoding ------------------------------------------------------

test "decodeAt reads a one byte code point" {
    const d = impl.decodeAt("a", 0);
    try testing.expectEqual(@as(u21, 'a'), d.cp);
    try testing.expectEqual(@as(usize, 1), d.len);
}

test "decodeAt reads a two byte code point" {
    const d = impl.decodeAt("\u{00a0}x", 0);
    try testing.expectEqual(@as(u21, 0xA0), d.cp);
    try testing.expectEqual(@as(usize, 2), d.len);
}

test "decodeAt reads a three byte code point" {
    const d = impl.decodeAt("\u{3000}", 0);
    try testing.expectEqual(@as(u21, 0x3000), d.cp);
    try testing.expectEqual(@as(usize, 3), d.len);
}

test "decodeAt reads a four byte code point" {
    const d = impl.decodeAt("\u{1f600}", 0);
    try testing.expectEqual(@as(u21, 0x1F600), d.cp);
    try testing.expectEqual(@as(usize, 4), d.len);
}

test "decodeAt falls back to the raw byte on a malformed lead" {
    const d = impl.decodeAt("\xff", 0);
    try testing.expectEqual(@as(u21, 0xFF), d.cp);
    try testing.expectEqual(@as(usize, 1), d.len);
}

test "decodeAt falls back to the raw byte on a truncated sequence" {
    const d = impl.decodeAt("\xe2\x80", 0);
    try testing.expectEqual(@as(u21, 0xE2), d.cp);
    try testing.expectEqual(@as(usize, 1), d.len);
}

// -- whitespace and stripping -------------------------------------------------

test "isSpaceAt accepts the ASCII space" {
    try testing.expect(impl.isSpaceAt(" ", 0));
}

test "isSpaceAt accepts tab and newline" {
    try testing.expect(impl.isSpaceAt("\t", 0));
    try testing.expect(impl.isSpaceAt("\n", 0));
}

test "isSpaceAt accepts the no break space" {
    try testing.expect(impl.isSpaceAt("\u{00a0}", 0));
}

test "isSpaceAt accepts the ideographic space" {
    try testing.expect(impl.isSpaceAt("\u{3000}", 0));
}

test "isSpaceAt accepts the file separator controls" {
    try testing.expect(impl.isSpaceAt("\x1c", 0));
    try testing.expect(impl.isSpaceAt("\x1f", 0));
}

test "isSpaceAt rejects a letter" {
    try testing.expect(!impl.isSpaceAt("a", 0));
}

test "allSpace holds for the empty string" {
    try testing.expect(impl.allSpace(""));
}

test "allSpace holds for a mixed whitespace run" {
    try testing.expect(impl.allSpace(" \t\u{00a0}\u{3000}"));
}

test "allSpace fails on any non whitespace code point" {
    try testing.expect(!impl.allSpace("  x  "));
}

test "lstrip removes leading whitespace only" {
    try testing.expectEqualStrings("x  ", impl.lstrip("  x  "));
}

test "rstrip removes trailing whitespace only" {
    try testing.expectEqualStrings("  x", impl.rstrip("  x  "));
}

test "strip removes both ends" {
    try testing.expectEqualStrings("x", impl.strip("  x  "));
}

test "strip takes unicode whitespace the way str.strip does" {
    try testing.expectEqualStrings("x", impl.strip("\u{00a0}\u{2028}x\u{3000}"));
}

test "strip of an all whitespace string is empty" {
    try testing.expectEqualStrings("", impl.strip(" \t\n"));
}

test "strip leaves an already tight string alone" {
    try testing.expectEqualStrings("cmake -S x", impl.strip("cmake -S x"));
}

// -- str.splitlines -----------------------------------------------------------

test "LineIterator splits on the line feed" {
    const lines = try collectLines(testing.allocator, "a\nb");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 2), lines.len);
    try testing.expectEqualStrings("a", lines[0]);
    try testing.expectEqualStrings("b", lines[1]);
}

test "LineIterator yields no trailing empty line" {
    const lines = try collectLines(testing.allocator, "a\nb\n");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 2), lines.len);
}

test "LineIterator treats CRLF as one break" {
    const lines = try collectLines(testing.allocator, "a\r\nb");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 2), lines.len);
    try testing.expectEqualStrings("a", lines[0]);
}

test "LineIterator breaks on a lone carriage return" {
    const lines = try collectLines(testing.allocator, "a\rb");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 2), lines.len);
}

test "LineIterator breaks on the vertical tab" {
    const lines = try collectLines(testing.allocator, "a\x0bb");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 2), lines.len);
}

test "LineIterator breaks on the form feed" {
    const lines = try collectLines(testing.allocator, "a\x0cb");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 2), lines.len);
}

test "LineIterator breaks on the file group and record separators" {
    const lines = try collectLines(testing.allocator, "a\x1cb\x1dc\x1ed");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 4), lines.len);
}

test "LineIterator does not break on the unit separator" {
    const lines = try collectLines(testing.allocator, "a\x1fb");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 1), lines.len);
}

test "LineIterator breaks on U+0085" {
    const lines = try collectLines(testing.allocator, "a\u{0085}b");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 2), lines.len);
}

test "LineIterator breaks on U+2028 and U+2029" {
    const lines = try collectLines(testing.allocator, "a\u{2028}b\u{2029}c");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 3), lines.len);
}

test "LineIterator yields nothing for empty text" {
    const lines = try collectLines(testing.allocator, "");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 0), lines.len);
}

test "LineIterator keeps interior blank lines" {
    const lines = try collectLines(testing.allocator, "a\n\nb");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 3), lines.len);
    try testing.expectEqualStrings("", lines[1]);
}

// -- RECIPE -------------------------------------------------------------------

test "matchRecipe accepts a bare recipe header" {
    try testing.expectEqualStrings("build", impl.matchRecipe("build:").?);
}

test "matchRecipe is anchored, so an indented line is never a header" {
    try testing.expect(impl.matchRecipe("    build:") == null);
}

test "matchRecipe accepts a space before the colon" {
    try testing.expectEqualStrings("build", impl.matchRecipe("build :").?);
}

test "matchRecipe accepts parameters between the name and the colon" {
    try testing.expectEqualStrings("build", impl.matchRecipe("build tool=\"all\":").?);
}

test "matchRecipe accepts a hyphenated recipe name" {
    try testing.expectEqualStrings("build-all", impl.matchRecipe("build-all:").?);
}

test "matchRecipe accepts an underscore start" {
    try testing.expectEqualStrings("_private", impl.matchRecipe("_private:").?);
}

test "matchRecipe rejects a digit start" {
    try testing.expect(impl.matchRecipe("2fast:") == null);
}

test "matchRecipe rejects a body line that merely contains a colon" {
    try testing.expect(impl.matchRecipe("b: x") == null);
}

test "matchRecipe tolerates trailing whitespace after the colon" {
    try testing.expectEqualStrings("build", impl.matchRecipe("build:   ").?);
}

test "matchRecipe takes a no break space as the parameter separator" {
    try testing.expectEqualStrings("build", impl.matchRecipe("build\u{00a0}:").?);
}

test "matchRecipe takes U+2028 as the parameter separator" {
    try testing.expectEqualStrings("build", impl.matchRecipe("build\u{2028}:").?);
}

test "matchRecipe rejects a line with no colon" {
    try testing.expect(impl.matchRecipe("build") == null);
}

test "matchRecipe rejects a colon followed by a word" {
    try testing.expect(impl.matchRecipe("build:x") == null);
}

test "matchRecipe rejects the empty line" {
    try testing.expect(impl.matchRecipe("") == null);
}

test "matchRecipe keeps the whole name, not a prefix of it" {
    try testing.expectEqualStrings("buildx", impl.matchRecipe("buildx:").?);
}

// -- recipe bodies ------------------------------------------------------------

test "recipeBodies splits consecutive recipes" {
    const recipes = try impl.recipeBodies(testing.allocator, "a:\n    one\nb:\n    two\n");
    defer impl.freeRecipes(testing.allocator, recipes);
    try testing.expectEqual(@as(usize, 2), recipes.len);
    try testing.expectEqualStrings("    one", bodyOf(recipes, "a").?);
    try testing.expectEqualStrings("    two", bodyOf(recipes, "b").?);
}

test "recipeBodies keeps a non indented non header line from closing a recipe" {
    const recipes = try impl.recipeBodies(testing.allocator, "a:\n    one\nnotheader\n    two\n");
    defer impl.freeRecipes(testing.allocator, recipes);
    try testing.expectEqual(@as(usize, 1), recipes.len);
    try testing.expectEqualStrings("    one\n    two", recipes[0].body);
}

test "recipeBodies drops a comment line inside a body" {
    const recipes = try impl.recipeBodies(testing.allocator, "a:\n    # note\n    one\n");
    defer impl.freeRecipes(testing.allocator, recipes);
    try testing.expectEqualStrings("    one", recipes[0].body);
}

test "recipeBodies keeps a blank line inside a body" {
    const recipes = try impl.recipeBodies(testing.allocator, "a:\n    one\n\n    two\n");
    defer impl.freeRecipes(testing.allocator, recipes);
    try testing.expectEqualStrings("    one\n\n    two", recipes[0].body);
}

test "recipeBodies accepts tab indentation" {
    const recipes = try impl.recipeBodies(testing.allocator, "a:\n\tone\n");
    defer impl.freeRecipes(testing.allocator, recipes);
    try testing.expectEqualStrings("\tone", recipes[0].body);
}

test "recipeBodies finds nothing in a module with no recipes" {
    const recipes = try impl.recipeBodies(testing.allocator, "# only a comment\n");
    defer impl.freeRecipes(testing.allocator, recipes);
    try testing.expectEqual(@as(usize, 0), recipes.len);
}

test "recipeBodies ignores lines before the first header" {
    const recipes = try impl.recipeBodies(testing.allocator, "    stray\na:\n    one\n");
    defer impl.freeRecipes(testing.allocator, recipes);
    try testing.expectEqual(@as(usize, 1), recipes.len);
    try testing.expectEqualStrings("    one", recipes[0].body);
}

test "recipeBodies flushes the last recipe at end of text" {
    const recipes = try impl.recipeBodies(testing.allocator, "a:\n    one");
    defer impl.freeRecipes(testing.allocator, recipes);
    try testing.expectEqual(@as(usize, 1), recipes.len);
}

test "recipeBodies gives a header with no body an empty body" {
    const recipes = try impl.recipeBodies(testing.allocator, "a:\nb:\n    two\n");
    defer impl.freeRecipes(testing.allocator, recipes);
    try testing.expectEqual(@as(usize, 2), recipes.len);
    try testing.expectEqualStrings("", bodyOf(recipes, "a").?);
}

test "recipeBodies strips the captured recipe name" {
    const recipes = try impl.recipeBodies(testing.allocator, "build tool:\n    one\n");
    defer impl.freeRecipes(testing.allocator, recipes);
    try testing.expectEqualStrings("build", recipes[0].name);
}

// -- shell commands -----------------------------------------------------------

test "shellCommands yields one command per line" {
    try testing.expectEqual(@as(usize, 2), try commandCount("    one\n    two\n"));
}

test "shellCommands joins a backslash continuation" {
    const commands = try impl.shellCommands(testing.allocator, "    gcc a.c \\\n    -o x\n");
    defer impl.freeStrings(testing.allocator, commands);
    try testing.expectEqual(@as(usize, 1), commands.len);
    try testing.expectEqualStrings("gcc a.c -o x", commands[0]);
}

test "shellCommands ignores blank lines" {
    try testing.expectEqual(@as(usize, 1), try commandCount("\n    one\n\n"));
}

test "shellCommands flushes a body that ends on a continuation" {
    const commands = try impl.shellCommands(testing.allocator, "    gcc a.c \\\n");
    defer impl.freeStrings(testing.allocator, commands);
    try testing.expectEqual(@as(usize, 1), commands.len);
    try testing.expectEqualStrings("gcc a.c", commands[0]);
}

test "shellCommands yields nothing for an empty body" {
    try testing.expectEqual(@as(usize, 0), try commandCount(""));
}

test "shellCommands strips each line before joining" {
    const commands = try impl.shellCommands(testing.allocator, "      cmake -S x   \n");
    defer impl.freeStrings(testing.allocator, commands);
    try testing.expectEqualStrings("cmake -S x", commands[0]);
}

test "shellCommands joins two consecutive continuations" {
    const commands = try impl.shellCommands(testing.allocator, "    a \\\n    b \\\n    c\n");
    defer impl.freeStrings(testing.allocator, commands);
    try testing.expectEqual(@as(usize, 1), commands.len);
    try testing.expectEqualStrings("a b c", commands[0]);
}

test "shellCommands keeps an interior hash, which is not a comment here" {
    const commands = try impl.shellCommands(testing.allocator, "    echo '# hi'\n");
    defer impl.freeStrings(testing.allocator, commands);
    try testing.expectEqualStrings("echo '# hi'", commands[0]);
}

// -- RAW_CMAKE_CONFIGURE ------------------------------------------------------

test "rawCmakeConfigure fires on a native configure" {
    try testing.expect(impl.rawCmakeConfigure("cmake -S x -B b"));
}

test "rawCmakeConfigure stays quiet on a build invocation" {
    try testing.expect(!impl.rawCmakeConfigure("cmake --build b"));
}

test "rawCmakeConfigure stays quiet on an install invocation" {
    try testing.expect(!impl.rawCmakeConfigure("cmake --install ."));
}

test "rawCmakeConfigure fires on two spaces before --build, because the run gives ground back" {
    try testing.expect(impl.rawCmakeConfigure("cmake  --build b"));
}

test "rawCmakeConfigure needs whitespace after the token" {
    try testing.expect(!impl.rawCmakeConfigure("cmake--build"));
}

test "rawCmakeConfigure fires on --buildy, because the boundary fails on a word character" {
    try testing.expect(impl.rawCmakeConfigure("cmake --buildy b"));
}

test "rawCmakeConfigure fires on --installx for the same reason" {
    try testing.expect(impl.rawCmakeConfigure("cmake --installx"));
}

test "rawCmakeConfigure fires mid command after whitespace" {
    try testing.expect(impl.rawCmakeConfigure("bash -c cmake -S x"));
}

test "rawCmakeConfigure needs a whitespace boundary, not a word one" {
    try testing.expect(!impl.rawCmakeConfigure("xcmake -S x"));
}

test "rawCmakeConfigure stays quiet on the bare token" {
    try testing.expect(!impl.rawCmakeConfigure("cmake"));
}

test "rawCmakeConfigure fires on a trailing whitespace run" {
    try testing.expect(impl.rawCmakeConfigure("cmake "));
}

test "rawCmakeConfigure takes a newline as the separator" {
    try testing.expect(impl.rawCmakeConfigure("cmake\n-S x"));
}

test "rawCmakeConfigure takes a no break space as the separator" {
    try testing.expect(impl.rawCmakeConfigure("cmake\u{00a0}-S x"));
}

test "rawCmakeConfigure takes a tab as the separator" {
    try testing.expect(impl.rawCmakeConfigure("cmake\t-S x"));
}

test "rawCmakeConfigure fires on two tabs before --build" {
    try testing.expect(impl.rawCmakeConfigure("cmake\t\t--build b"));
}

// -- RAW_COMPILER -------------------------------------------------------------

test "rawCompiler fires on a standard driver invocation" {
    try testing.expect(impl.rawCompiler("cc -std=gnu23 src/main.c -o tool"));
}

test "rawCompiler stays quiet on gcc -c, whose separator the run consumed" {
    try testing.expect(!impl.rawCompiler("gcc -c a.c"));
}

test "rawCompiler fires on a double space before -c" {
    try testing.expect(impl.rawCompiler("cc  -c "));
}

test "rawCompiler stays quiet on clang-17 -o, for the same separator reason" {
    try testing.expect(!impl.rawCompiler("clang-17 -o x a.c"));
}

test "rawCompiler fires across a line break, because the class takes a newline" {
    try testing.expect(impl.rawCompiler("gcc a.c\n-o x"));
}

test "rawCompiler fires on a braced make variable" {
    try testing.expect(impl.rawCompiler("${cc} -std=c23 x.c"));
}

test "rawCompiler fires on a bare shell variable with a later -o" {
    try testing.expect(impl.rawCompiler("$cc -c x.c -o y"));
}

test "rawCompiler fires on a versioned gcc with -std=" {
    try testing.expect(impl.rawCompiler("gcc-13 -std=c23 x.c"));
}

test "rawCompiler fires on plain clang with -std=" {
    try testing.expect(impl.rawCompiler("clang -std=c23 x.c"));
}

test "rawCompiler stays quiet on the wrapper the gate wants" {
    try testing.expect(!impl.rawCompiler("bash scripts/builders/host_cmake.sh tools/x tools/x/build"));
}

test "rawCompiler stays quiet on empty text" {
    try testing.expect(!impl.rawCompiler(""));
}

test "rawCompiler stays quiet on a bare compiler token" {
    try testing.expect(!impl.rawCompiler("cc"));
}

test "rawCompiler needs the token to end, so cco is not cc" {
    try testing.expect(!impl.rawCompiler("cco -std=c23 x"));
}

test "rawCompiler fires when the driver sits mid command" {
    try testing.expect(impl.rawCompiler("env cc -std=c23 x.c"));
}

// -- accumulated recipe findings ---------------------------------------------

test "recipeErrors accepts the wrapper body" {
    try testing.expectEqual(@as(usize, 0), try errorCount(
        "good",
        "build:\n    bash scripts/builders/host_cmake.sh tools/x tools/x/build\n",
    ));
}

test "recipeErrors accepts an ARM configure carrying a toolchain file" {
    try testing.expectEqual(@as(usize, 0), try errorCount(
        "cross",
        "build:\n    cmake -S x -B b -DCMAKE_TOOLCHAIN_FILE=cmake/arm.cmake\n    cmake --build b\n",
    ));
}

test "recipeErrors rejects a raw native configure" {
    try testing.expectEqual(@as(usize, 1), try errorCount(
        "bad",
        "build:\n    cmake -S tools/x -B tools/x/build\n",
    ));
}

test "recipeErrors rejects a raw native configure hidden beside an ARM one" {
    try testing.expectEqual(@as(usize, 1), try errorCount(
        "mixed",
        "build:\n    cmake -S arm -B arm/build -DCMAKE_TOOLCHAIN_FILE=cmake/arm.cmake\n    cmake -S tools/x -B tools/x/build\n",
    ));
}

test "recipeErrors rejects a raw compile driver" {
    try testing.expectEqual(@as(usize, 1), try errorCount(
        "cc",
        "build:\n    cc -std=gnu23 src/main.c -o tool\n",
    ));
}

test "recipeErrors names the module and the recipe in its message" {
    const message = (try firstError(testing.allocator, "just/tools.just", "build:\n    cmake -S x -B b\n")).?;
    defer testing.allocator.free(message);
    try testing.expect(std.mem.indexOf(u8, message, "just/tools.just") != null);
    try testing.expect(std.mem.indexOf(u8, message, "recipe build") != null);
    try testing.expect(std.mem.indexOf(u8, message, "bypasses host_cmake.sh") != null);
}

test "recipeErrors names the compile driver rule in its own message" {
    const message = (try firstError(testing.allocator, "justfile", "t:\n    cc  -c x.c\n")).?;
    defer testing.allocator.free(message);
    try testing.expect(std.mem.indexOf(u8, message, "raw host compiler invocation bypasses CMake") != null);
}

test "recipeErrors accumulates across recipes" {
    try testing.expectEqual(@as(usize, 2), try errorCount(
        "two",
        "a:\n    cmake -S x -B b\nb:\n    cmake -S y -B c\n",
    ));
}

test "recipeErrors accepts a comment only body" {
    try testing.expectEqual(@as(usize, 0), try errorCount("c", "a:\n    # cmake -S x -B b\n"));
}

test "recipeErrors accepts a module with no recipes at all" {
    try testing.expectEqual(@as(usize, 0), try errorCount("empty", "# nothing here\n"));
}

// -- standalone CMake ---------------------------------------------------------

test "standaloneCmake accepts a top level project declaration" {
    try testing.expect(impl.standaloneCmake("project(shared LANGUAGES C)\n"));
}

test "standaloneCmake rejects a consumer fragment" {
    try testing.expect(!impl.standaloneCmake("target_sources(app PRIVATE src/x.c)\n"));
}

test "standaloneCmake accepts an indented project declaration" {
    try testing.expect(impl.standaloneCmake("    project(x)\n"));
}

test "standaloneCmake accepts whitespace before the parenthesis" {
    try testing.expect(impl.standaloneCmake("project (x)\n"));
}

test "standaloneCmake does not match a name ending in project" {
    try testing.expect(!impl.standaloneCmake("myproject(x)\n"));
}

test "standaloneCmake looks past the first line" {
    try testing.expect(impl.standaloneCmake("cmake_minimum_required(VERSION 3.25)\nproject(x)\n"));
}

test "standaloneCmake needs the parenthesis" {
    try testing.expect(!impl.standaloneCmake("project\n"));
}

test "standaloneCmake rejects empty text" {
    try testing.expect(!impl.standaloneCmake(""));
}

// -- path suffixes ------------------------------------------------------------

test "pathlibSuffix reads an ordinary suffix" {
    try testing.expectEqualStrings(".c", impl.pathlibSuffix("main.c"));
}

test "pathlibSuffix gives a dotfile no suffix" {
    try testing.expectEqualStrings("", impl.pathlibSuffix(".bashrc"));
}

test "pathlibSuffix gives a trailing dot no suffix" {
    try testing.expectEqualStrings("", impl.pathlibSuffix("trailing."));
}

test "pathlibSuffix takes the last suffix only" {
    try testing.expectEqualStrings(".cpp", impl.pathlibSuffix("a.b.cpp"));
}

test "pathlibSuffix gives an extensionless name no suffix" {
    try testing.expectEqualStrings("", impl.pathlibSuffix("noext"));
}

test "isCompiledSuffix accepts every authored compiled suffix" {
    try testing.expect(impl.isCompiledSuffix("a.c"));
    try testing.expect(impl.isCompiledSuffix("a.cc"));
    try testing.expect(impl.isCompiledSuffix("a.cpp"));
    try testing.expect(impl.isCompiledSuffix("a.cxx"));
    try testing.expect(impl.isCompiledSuffix("a.m"));
    try testing.expect(impl.isCompiledSuffix("a.mm"));
}

test "isCompiledSuffix rejects a header" {
    try testing.expect(!impl.isCompiledSuffix("a.h"));
}

test "isCompiledSuffix rejects a Zig source" {
    try testing.expect(!impl.isCompiledSuffix("a.zig"));
}

test "isCompiledSuffix rejects a file named exactly .c, as pathlib does" {
    try testing.expect(!impl.isCompiledSuffix(".c"));
}

// -- small helpers ------------------------------------------------------------

test "containsString finds a member" {
    try testing.expect(impl.containsString(&.{ "a", "b" }, "b"));
}

test "containsString rejects a non member" {
    try testing.expect(!impl.containsString(&.{ "a", "b" }, "c"));
}

test "lessThanString orders lexicographically" {
    try testing.expect(impl.lessThanString({}, "a", "b"));
    try testing.expect(!impl.lessThanString({}, "b", "a"));
}

// -- dispatcher coverage, both ways ------------------------------------------

test "dispatcherFixtureErrors reports an omission" {
    const errors = try impl.dispatcherFixtureErrors(testing.allocator, &.{"native"}, &.{});
    defer impl.freeStrings(testing.allocator, errors);
    try testing.expectEqual(@as(usize, 1), errors.len);
    try testing.expectEqualStrings("missing native", errors[0]);
}

test "dispatcherFixtureErrors reports an extra listing" {
    const errors = try impl.dispatcherFixtureErrors(testing.allocator, &.{}, &.{"ghost"});
    defer impl.freeStrings(testing.allocator, errors);
    try testing.expectEqual(@as(usize, 1), errors.len);
    try testing.expectEqualStrings("extra ghost", errors[0]);
}

test "dispatcherFixtureErrors reports both directions at once" {
    const errors = try impl.dispatcherFixtureErrors(testing.allocator, &.{"a"}, &.{"b"});
    defer impl.freeStrings(testing.allocator, errors);
    try testing.expectEqual(@as(usize, 2), errors.len);
}

test "dispatcherFixtureErrors accepts an exact match" {
    const errors = try impl.dispatcherFixtureErrors(testing.allocator, &.{ "a", "b" }, &.{ "b", "a" });
    defer impl.freeStrings(testing.allocator, errors);
    try testing.expectEqual(@as(usize, 0), errors.len);
}

test "dispatcherFixtureErrors accepts two empty sets" {
    const errors = try impl.dispatcherFixtureErrors(testing.allocator, &.{}, &.{});
    defer impl.freeStrings(testing.allocator, errors);
    try testing.expectEqual(@as(usize, 0), errors.len);
}
