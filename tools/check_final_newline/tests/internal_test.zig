//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the trailing-newline gate's scope and
//! detector algebra (#858). Every case pins behaviour inherited from the
//! Python this replaced, not behaviour invented here.

const std = @import("std");
const implementation = @import("implementation");

test "detector stays quiet on a newline-terminated file" {
    try std.testing.expect(implementation.endsInNewline("x = 1\n"));
}

test "detector stays quiet on an empty file" {
    try std.testing.expect(implementation.endsInNewline(""));
}

test "detector fires on a file with no trailing newline" {
    try std.testing.expect(!implementation.endsInNewline("x = 1"));
}

test "detector reads the last byte, not the presence of newlines" {
    try std.testing.expect(!implementation.endsInNewline("first\nsecond"));
}

test "a trailing carriage return is not a trailing newline" {
    try std.testing.expect(!implementation.endsInNewline("x = 1\r"));
}

test "a CRLF terminator ends in a newline byte" {
    try std.testing.expect(implementation.endsInNewline("x = 1\r\n"));
}

test "build directory names require the separator" {
    try std.testing.expect(implementation.isBuildDirName("build"));
    try std.testing.expect(implementation.isBuildDirName("build-cov"));
    try std.testing.expect(implementation.isBuildDirName("build_host"));
    try std.testing.expect(implementation.isBuildDirName("cmake-build-debug"));
}

test "builders is not a build directory" {
    try std.testing.expect(!implementation.isBuildDirName("builders"));
    try std.testing.expect(!implementation.isBuildDirName("buildsystem"));
}

test "a build tree under a product root is build output" {
    try std.testing.expect(implementation.isBuildOutput("tools/demo/build/object.o"));
    try std.testing.expect(implementation.isBuildOutput("tests/build-cov/report.c"));
}

test "a build directory at the repository root is build output" {
    try std.testing.expect(implementation.isBuildOutput("build/config.cmake"));
}

test "a source directory called build outside a product root stays visible" {
    try std.testing.expect(!implementation.isBuildOutput("scripts/build/helper.sh"));
    try std.testing.expect(!implementation.isBuildOutput("internal/build/helper.sh"));
}

test "tool-owned directory names are excluded at any depth" {
    try std.testing.expect(implementation.isBuildOutput("apps/host/reg_gen/.zig-cache/cache.zig"));
    try std.testing.expect(implementation.isBuildOutput("scripts/checks/__pycache__/x.py"));
    try std.testing.expect(implementation.isBuildOutput("a/b/c/CMakeFiles/link.txt"));
    try std.testing.expect(implementation.isBuildOutput("x/node_modules/y/index.yml"));
    try std.testing.expect(implementation.isBuildOutput("x/_deps/y/z.c"));
}

test "a FILE called build is not a build tree" {
    try std.testing.expect(!implementation.isBuildOutput("scripts/build"));
    try std.testing.expect(!implementation.isBuildOutput("scripts/dev/build"));
}

test "a single-component path is never build output" {
    try std.testing.expect(!implementation.isBuildOutput("justfile"));
    try std.testing.expect(!implementation.isBuildOutput("build"));
}

test "absolute build-output paths normalise against the root" {
    const allocator = std.testing.allocator;
    try std.testing.expect(try implementation.isBuildOutputPath(allocator, "/repo/tools/x/build/a.c", "/repo"));
    try std.testing.expect(!try implementation.isBuildOutputPath(allocator, "/repo/scripts/build/a.sh", "/repo"));
}

test "a dot-slash prefixed path normalises" {
    const allocator = std.testing.allocator;
    try std.testing.expect(try implementation.isBuildOutputPath(allocator, "./tools/x/build/a.c", "/elsewhere"));
}

test "path names split on the last separator" {
    try std.testing.expectEqualStrings("root.zig", implementation.pathName("src/internal/root.zig"));
    try std.testing.expectEqualStrings("justfile", implementation.pathName("justfile"));
}

test "suffixes follow pathlib: a leading dot is a name, not a suffix" {
    try std.testing.expectEqualStrings("", implementation.pathSuffix(".bashrc"));
    try std.testing.expectEqualStrings("", implementation.pathSuffix("trailing."));
    try std.testing.expectEqualStrings(".gz", implementation.pathSuffix("archive.tar.gz"));
    try std.testing.expectEqualStrings(".c", implementation.pathSuffix("driver.c"));
    try std.testing.expectEqualStrings("", implementation.pathSuffix("Makefile"));
}

test "language comes from the basename before the suffix" {
    try std.testing.expectEqualStrings("cmake", implementation.rawLanguage("port/threadx/CMakeLists.txt").?);
    try std.testing.expectEqualStrings("just", implementation.rawLanguage("just/tests.just").?);
    try std.testing.expectEqualStrings("c", implementation.rawLanguage("libs/ra8_mpu/src/mpu.c").?);
    try std.testing.expectEqualStrings("python", implementation.rawLanguage("scripts/dev/x.py").?);
}

test "a scanned suffix with no language entry answers null" {
    try std.testing.expect(implementation.rawLanguage("apps/x/objc.m") == null);
    try std.testing.expect(implementation.rawLanguage("libs/x/table.inl") == null);
}

test "an extensionless name answers null rather than reading a shebang" {
    try std.testing.expect(implementation.rawLanguage("scripts/git/pre-commit") == null);
}

test "vendored SOUP and generated tables are excluded for every language" {
    try std.testing.expect(implementation.isExcludedRel("libs/third_party/x/a.c", null));
    try std.testing.expect(implementation.isExcludedRel("apps/shared_libs/third_party/x/a.h", null));
    try std.testing.expect(implementation.isExcludedRel("libs/ra8_fonts/literata.h", null));
    try std.testing.expect(implementation.isExcludedRel("tools/vela/generated/model.c", null));
}

test "the threadx exclusion is C-only, so its build glue stays in scope" {
    try std.testing.expect(implementation.isExcludedRel("port/threadx/src/vendor.c", "c"));
    try std.testing.expect(!implementation.isExcludedRel("port/threadx/CMakeLists.txt", "cmake"));
    try std.testing.expect(!implementation.isExcludedRel("port/threadx/src/vendor.c", null));
}

test "first-party keeps ordinary source and drops vendored C" {
    try std.testing.expect(implementation.isFirstParty("libs/ra8_mpu/src/mpu.c"));
    try std.testing.expect(!implementation.isFirstParty("port/threadx/src/vendor.c"));
    try std.testing.expect(implementation.isFirstParty("port/threadx/CMakeLists.txt"));
}

test "source suffixes are matched by ending" {
    try std.testing.expect(implementation.hasSourceSuffix("a/b.yaml"));
    try std.testing.expect(implementation.hasSourceSuffix("a/b.ld"));
    try std.testing.expect(!implementation.hasSourceSuffix("a/b.md"));
    try std.testing.expect(!implementation.hasSourceSuffix("a/b.json"));
}

test "listfiles are matched by basename, not by ending" {
    try std.testing.expect(implementation.isSourceName("just/justfile"));
    try std.testing.expect(implementation.isSourceName("CMakeLists.txt"));
    try std.testing.expect(!implementation.isSourceName("docs/my-justfile"));
    try std.testing.expect(!implementation.isSourceName("docs/OtherCMakeLists.txt"));
}

test "argv source detection covers suffixes and listfiles" {
    try std.testing.expect(implementation.isSource("/repo/scripts/x.sh"));
    try std.testing.expect(implementation.isSource("/repo/justfile"));
    try std.testing.expect(!implementation.isSource("/repo/README.md"));
    try std.testing.expect(!implementation.isSource("/repo/notes"));
}

test "derived scope keeps first-party source and sorts it" {
    const allocator = std.testing.allocator;
    const census = [_][]const u8{
        "libs/ra8_ui/src/ui.c",
        "apps/board/reader/CMakeLists.txt",
        "docs/guide.md",
        "just/tests.just",
    };
    const scope = try implementation.derivedScope(allocator, &census);
    defer allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 3), scope.len);
    try std.testing.expectEqualStrings("apps/board/reader/CMakeLists.txt", scope[0]);
    try std.testing.expectEqualStrings("just/tests.just", scope[1]);
    try std.testing.expectEqualStrings("libs/ra8_ui/src/ui.c", scope[2]);
}

test "derived scope drops build output and vendored trees" {
    const allocator = std.testing.allocator;
    const census = [_][]const u8{
        "tools/demo/build/generated.c",
        "libs/third_party/lz4/lz4.c",
        "libs/ra8_fonts/literata.h",
        "scripts/build/helper.sh",
    };
    const scope = try implementation.derivedScope(allocator, &census);
    defer allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 1), scope.len);
    try std.testing.expectEqualStrings("scripts/build/helper.sh", scope[0]);
}

test "derived scope drops a name-suffixed impostor listfile" {
    const allocator = std.testing.allocator;
    const census = [_][]const u8{ "docs/my-justfile", "just/justfile" };
    const scope = try implementation.derivedScope(allocator, &census);
    defer allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 1), scope.len);
    try std.testing.expectEqualStrings("just/justfile", scope[0]);
}

test "derived scope does not hide dot-prefixed paths" {
    const allocator = std.testing.allocator;
    const census = [_][]const u8{ ".github/workflows/ci.yml", ".clang-format" };
    const scope = try implementation.derivedScope(allocator, &census);
    defer allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 1), scope.len);
    try std.testing.expectEqualStrings(".github/workflows/ci.yml", scope[0]);
}

test "derived scope deduplicates a repeated census entry" {
    const allocator = std.testing.allocator;
    const census = [_][]const u8{ "libs/a/x.c", "libs/a/x.c" };
    const scope = try implementation.derivedScope(allocator, &census);
    defer allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 1), scope.len);
}

test "derived scope keeps vendored threadx build glue but not its C" {
    const allocator = std.testing.allocator;
    const census = [_][]const u8{ "port/threadx/src/tx.c", "port/threadx/CMakeLists.txt" };
    const scope = try implementation.derivedScope(allocator, &census);
    defer allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 1), scope.len);
    try std.testing.expectEqualStrings("port/threadx/CMakeLists.txt", scope[0]);
}

test "the gate's own fragments match anywhere in an absolute path" {
    try std.testing.expect(implementation.hasExcludedFragment("/repo/port/threadx/CMakeLists.txt"));
    try std.testing.expect(implementation.hasExcludedFragment("/repo/apps/_unsupported/x.c"));
    try std.testing.expect(!implementation.hasExcludedFragment("/repo/libs/ra8_ui/src/ui.c"));
}

test "the fragment subtraction is what removes threadx build glue from this gate" {
    try std.testing.expect(implementation.isFirstParty("port/threadx/CMakeLists.txt"));
    try std.testing.expect(implementation.hasExcludedFragment("/repo/port/threadx/CMakeLists.txt"));
}

test "display paths are repo-relative under the root and untouched outside it" {
    try std.testing.expectEqualStrings("libs/a/x.c", implementation.displayPath("/repo/libs/a/x.c", "/repo"));
    try std.testing.expectEqualStrings("/other/x.c", implementation.displayPath("/other/x.c", "/repo"));
}

test "a trailing slash on the root does not leak into the display path" {
    try std.testing.expectEqualStrings("libs/a/x.c", implementation.displayPath("/repo/libs/a/x.c", "/repo/"));
}

test "paths sort in byte order" {
    var paths = [_][]const u8{ "b/x.c", "a/z.c", "a/a.c" };
    implementation.sortPaths(&paths);
    try std.testing.expectEqualStrings("a/a.c", paths[0]);
    try std.testing.expectEqualStrings("a/z.c", paths[1]);
    try std.testing.expectEqualStrings("b/x.c", paths[2]);
}

test "the scope probe wants a directory, not a prefix" {
    const scope = [_][]const u8{ "just/tests.just", "infrastructure/x.yml" };
    try std.testing.expect(implementation.scopeReaches(&scope, "just"));
    try std.testing.expect(!implementation.scopeReaches(&scope, "infra"));
}

test "the scope probe finds the roots a hardcoded list had dropped" {
    const scope = [_][]const u8{ "just/tests.just", "infra/ansible/site.yml" };
    try std.testing.expect(implementation.scopeReaches(&scope, "just"));
    try std.testing.expect(implementation.scopeReaches(&scope, "infra"));
}

test "the floors are the inherited values" {
    try std.testing.expectEqual(@as(usize, 2200), implementation.file_floor);
    try std.testing.expectEqual(@as(usize, 1000), implementation.tracked_floor);
}
