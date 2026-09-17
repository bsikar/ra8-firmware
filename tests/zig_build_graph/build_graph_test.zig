//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the root build graph's own rules (#857).
//!
//! The graph encodes parity facts that a directory listing cannot tell you:
//! which board translation units are opt-in rather than universal, which
//! warning classes the vendored decoder may suppress and which it may not, and
//! how a compile command is escaped into the database the analysis gates parse.
//! Each of those is ordinary data or an ordinary function, so it is tested here
//! directly instead of only being exercised the long way round through a build.

const std = @import("std");
const graph = @import("build_graph");
const abi = graph.abi_contract;
const db = graph.compile_db;
const sources = graph.cross_sources;
const cpu1 = graph.cpu1_image;
const mw = graph.middleware;

/// The apps the cross slice builds, by the rules they exercise: one that names
/// no libraries at all, one that names two, and one that keeps more than a
/// single translation unit under its own `src/`.
const bare_app = graph.cross_apps[0];
const library_app = graph.cross_apps[1];
const dual_core_app = graph.cross_apps[2];
/// And one that names a vendored middleware in `USES`, which is the only way
/// the middleware exports below are observable at all.
const middleware_app = graph.cross_apps[3];

test "board opt-in gate drops the two sources an app must ask for" {
    try std.testing.expect(
        sources.isGatedOutBoardSource(bare_app, "libs/ra8_board_ek_ra8d2/src/ra8_board_console_stream.c"),
    );
    try std.testing.expect(
        sources.isGatedOutBoardSource(bare_app, "libs/ra8_board_ek_ra8d2/src/ra8_board_touch.c"),
    );
}

test "board opt-in gate keeps the universal board sources" {
    try std.testing.expect(
        !sources.isGatedOutBoardSource(bare_app, "libs/ra8_board_ek_ra8d2/src/ra8_board_clock.c"),
    );
    try std.testing.expect(
        !sources.isGatedOutBoardSource(bare_app, "libs/ra8_board_ek_ra8d2/src/ra8_board_pins.c"),
    );
    // The gate matches a suffix, not a substring: a source that merely mentions
    // the gated name must still be compiled.
    try std.testing.expect(
        !sources.isGatedOutBoardSource(bare_app, "libs/ra8_board_ek_ra8d2/src/ra8_board_touch_probe.c"),
    );
}

test "board opt-in gate opens for the app that names the library" {
    // The other arm of the same rule, and the reason the cross slice builds two
    // apps: iic_b_facade_demo names ra8_io_bus, which satisfies the touch gate,
    // so the SAME source is kept here and dropped above.
    try std.testing.expect(
        !sources.isGatedOutBoardSource(library_app, "libs/ra8_board_ek_ra8d2/src/ra8_board_touch.c"),
    );
    // ra8_io_bus does NOT satisfy the console-stream gate: that one hands back
    // an ra8_io_stream_t and needs the full ra8_io at link time.
    try std.testing.expect(
        sources.isGatedOutBoardSource(library_app, "libs/ra8_board_ek_ra8d2/src/ra8_board_console_stream.c"),
    );
}

test "a library with no directory of its own still contributes sources" {
    // ra8_io_bus has no libs/ra8_io_bus at all, so a graph built from the
    // directory listing compiles nothing for it and the link fails 200 TUs
    // later. The alias is what makes it six real translation units out of
    // libs/ra8_io, plus that library's include directory.
    const alias = sources.library_aliases[0];
    try std.testing.expectEqualStrings("ra8_io_bus", alias.name);
    try std.testing.expectEqualStrings("libs/ra8_io/src", alias.source_dir);
    try std.testing.expectEqualStrings("libs/ra8_io/inc", alias.include_dir);
    try std.testing.expect(sources.declaresLibrary(library_app, "ra8_io_bus"));
    try std.testing.expect(!sources.declaresLibrary(bare_app, "ra8_io_bus"));

    // And it is skipped for an app that names the fuller library, which
    // already compiles the same TUs -- compiling them twice is a duplicate
    // symbol at the link, not a warning.
    try std.testing.expectEqualStrings("ra8_io", alias.superseded_by[0]);
}

test "host C bar keeps -Werror and both off-target definitions" {
    var has_werror = false;
    var has_off_target = false;
    var has_unit_test = false;
    for (graph.c_flags) |flag| {
        if (std.mem.eql(u8, flag, "-Werror")) has_werror = true;
        if (std.mem.eql(u8, flag, "-DRA8_OFF_TARGET")) has_off_target = true;
        if (std.mem.eql(u8, flag, "-DUNIT_TEST")) has_unit_test = true;
    }
    try std.testing.expect(has_werror);
    try std.testing.expect(has_off_target);
    try std.testing.expect(has_unit_test);
}

test "vendored suppression is narrow and ordered" {
    // -Wno-conversion has to come AFTER the -Wconversion the first-party bar
    // turns on, or the suppression suppresses nothing and the SOUP parity claim
    // is empty. Order is the whole content of the rule, so assert on it.
    var enabled_at: ?usize = null;
    var suppressed_at: ?usize = null;
    for (graph.vendored_soup_flags, 0..) |flag, index| {
        if (std.mem.eql(u8, flag, "-Wconversion")) enabled_at = index;
        if (std.mem.eql(u8, flag, "-Wno-conversion")) suppressed_at = index;
    }
    try std.testing.expect(enabled_at != null);
    try std.testing.expect(suppressed_at != null);
    try std.testing.expect(enabled_at.? < suppressed_at.?);

    // Everything else stays at the first-party bar on an attacker-facing
    // decoder: no blanket -w, and -Werror survives.
    for (graph.vendored_soup_flags) |flag| {
        try std.testing.expect(!std.mem.eql(u8, flag, "-w"));
    }
    var has_werror = false;
    for (graph.vendored_soup_flags) |flag| {
        if (std.mem.eql(u8, flag, "-Werror")) has_werror = true;
    }
    try std.testing.expect(has_werror);
}

test "compile database escapes the bytes JSON cannot carry raw" {
    var out = std.ArrayList(u8).init(std.testing.allocator);
    defer out.deinit();
    db.appendJsonString(&out, "a\"b\\c\nd\te");
    try std.testing.expectEqualStrings("\"a\\\"b\\\\c\\nd\\te\"", out.items);
}

test "compile database leaves an ordinary path untouched" {
    var out = std.ArrayList(u8).init(std.testing.allocator);
    defer out.deinit();
    db.appendJsonString(&out, "libs/ra8_core/src/ra8_log.c");
    try std.testing.expectEqualStrings("\"libs/ra8_core/src/ra8_log.c\"", out.items);
}

test "ABI negative control passes only on a failure that names its reason" {
    const layout = abi.negative_fixtures[0];
    try std.testing.expectEqual(abi.NegativeKind.layout, layout.kind);

    // The whole point of a negative control: a compile that SUCCEEDS is the
    // failure, not the pass.
    try std.testing.expectEqual(
        abi.NegativeOutcome.unexpected_success,
        abi.classifyNegative(false, layout.expected_diagnostic, layout.expected_diagnostic),
    );
    // And a failure for an unrelated reason -- a missing header, a mistyped
    // flag -- proves nothing about the boundary, so it is not a pass either.
    try std.testing.expectEqual(
        abi.NegativeOutcome.missing_diagnostic,
        abi.classifyNegative(true, "fatal error: 'ra8_err.h' file not found", layout.expected_diagnostic),
    );
    try std.testing.expectEqual(
        abi.NegativeOutcome.expected_failure,
        abi.classifyNegative(
            true,
            "negative_layout.c:11:1: error: static assertion failed: \"ABI contract fixture deliberately requires an incompatible layout\"",
            layout.expected_diagnostic,
        ),
    );
}

test "ABI layout control stops before the link, the symbol control does not" {
    const allocator = std.testing.allocator;
    for (abi.negative_fixtures) |fixture| {
        const arguments = abi.negativeArguments(allocator, "zig", fixture, "/tmp/libfixture.a", "/tmp/out");
        defer allocator.free(arguments);

        var has_dialect = false;
        var links_archive = false;
        var compile_only = false;
        for (arguments) |argument| {
            if (std.mem.eql(u8, argument, abi.negative_dialect_flag)) has_dialect = true;
            if (std.mem.eql(u8, argument, "/tmp/libfixture.a")) links_archive = true;
            if (std.mem.eql(u8, argument, "-c")) compile_only = true;
        }
        try std.testing.expect(has_dialect);
        switch (fixture.kind) {
            // A missing export is only missing at the link, and only against
            // the real archive.
            .missing_symbol => {
                try std.testing.expect(links_archive);
                try std.testing.expect(!compile_only);
            },
            // Layout drift is a compile-time assertion; linking it would only
            // add an unrelated undefined `main` to the diagnostics.
            .layout => {
                try std.testing.expect(!links_archive);
                try std.testing.expect(compile_only);
            },
        }
    }
}

test "ABI consumer keeps the warning bar CMake puts on its consumer" {
    var has_werror = false;
    for (abi.consumer_flags) |flag| {
        if (std.mem.eql(u8, flag, "-Werror")) has_werror = true;
        try std.testing.expect(!std.mem.eql(u8, flag, "-w"));
    }
    try std.testing.expect(has_werror);
    // The fixture's own public header and the shared error vocabulary, in the
    // order zig_abi_contract.cmake puts them on the include path.
    try std.testing.expectEqualStrings("tests/zig_abi_fixture/inc", abi.include_paths[0]);
    try std.testing.expectEqualStrings("libs/ra8_core/inc", abi.include_paths[1]);
}

test "AUX_SRCS keeps the second image's entry point out of this image" {
    // src/cpu1_main.c is an ordinary-looking app-local source that compiles
    // cleanly into the WRONG image: it is the Cortex-M33 entry point, built by
    // a second executable in the app's own CMakeLists. A graph that globs
    // <app>/src/*.c and stops there links it into the M85 image.
    try std.testing.expect(sources.isAuxSource(dual_core_app, "src/cpu1_main.c"));
    try std.testing.expect(!sources.appLocalIsCompiled(dual_core_app, "src/cpu1_main.c"));

    // Its sibling under the same directory is not aux and IS compiled.
    try std.testing.expect(!sources.isAuxSource(dual_core_app, "src/board_helper.c"));
    try std.testing.expect(sources.appLocalIsCompiled(dual_core_app, "src/board_helper.c"));

    // The rule is per app, not per filename: an app that never declared it
    // would compile a file of the same name.
    try std.testing.expect(!sources.isAuxSource(bare_app, "src/cpu1_main.c"));
    try std.testing.expect(sources.appLocalIsCompiled(bare_app, "src/cpu1_main.c"));
    try std.testing.expectEqual(@as(usize, 0), bare_app.aux_srcs.len);
    try std.testing.expectEqual(@as(usize, 0), library_app.aux_srcs.len);
}

test "the app-local glob does not re-add what is already in the link" {
    // main.c is the first object in the link, placed before the glob runs.
    try std.testing.expect(!sources.appLocalIsCompiled(dual_core_app, "src/main.c"));
    // A boot unit belongs to the per-app resolver below, which already chose
    // between this copy and the board's. Taking it again is a duplicate object.
    try std.testing.expect(!sources.appLocalIsCompiled(dual_core_app, "src/trustzone_init.c"));
    try std.testing.expect(!sources.appLocalIsCompiled(dual_core_app, "src/vector_table.c"));
    try std.testing.expect(!sources.appLocalIsCompiled(bare_app, "src/system_init.c"));
}

test "an app-local boot copy replaces the board copy, and only when it exists" {
    const allocator = std.testing.allocator;

    // cpu1_pingpong ships src/trustzone_init.c, so that unit resolves to the
    // app copy and the board's src/boot copy is not linked.
    const app_copy = sources.bootSourcePath(allocator, dual_core_app, "trustzone_init.c", true);
    defer allocator.free(app_copy);
    try std.testing.expectEqualStrings(
        "examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong/src/trustzone_init.c",
        app_copy,
    );

    // Its other four boot units, and all five of an app that ships none, come
    // from the board layer instead.
    const board_copy = sources.bootSourcePath(allocator, dual_core_app, "vector_table.c", false);
    defer allocator.free(board_copy);
    try std.testing.expectEqualStrings("libs/ra8_board_ek_ra8d2/src/boot/vector_table.c", board_copy);

    const bare_copy = sources.bootSourcePath(allocator, bare_app, "trustzone_init.c", false);
    defer allocator.free(bare_copy);
    try std.testing.expectEqualStrings("libs/ra8_board_ek_ra8d2/src/boot/trustzone_init.c", bare_copy);
}

test "the second image compiles exactly the four units its executable names" {
    const allocator = std.testing.allocator;
    const image = dual_core_app.cpu1 orelse return error.MissingCpu1Image;
    const app = cpu1App(dual_core_app);

    const units = cpu1.sources(allocator, app, image);
    defer allocator.free(units);
    defer allocator.free(units[0]);
    try std.testing.expectEqual(@as(usize, 4), units.len);
    try std.testing.expectEqualStrings(
        "examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong/src/cpu1_main.c",
        units[0],
    );
    try std.testing.expectEqualStrings("libs/ra8_hal/src/ra8_ipc.c", units[1]);

    // The entry unit is the same file AUX_SRCS keeps out of the M85 image: one
    // file, two images, and each rule is the other's mirror.
    try std.testing.expect(sources.isAuxSource(dual_core_app, image.entry_source));
    try std.testing.expect(!sources.appLocalIsCompiled(dual_core_app, image.entry_source));

    // A single-core app has no second image at all.
    try std.testing.expect(bare_app.cpu1 == null);
    try std.testing.expect(library_app.cpu1 == null);
}

test "the second image's flags override the inherited ones, in that order" {
    const allocator = std.testing.allocator;
    const global = [_][]const u8{ "-mcpu=cortex-m85", "-fdata-sections", "-O0", "-std=gnu2x" };
    const flags = cpu1.compileFlags(allocator, &global);
    defer allocator.free(flags);

    // gcc takes the last -mcpu and the last -O, so the inherited M85 flags
    // must come FIRST and the M33 target's own after them. Reversed, this
    // builds the second core's image for the first core's core.
    try std.testing.expect(indexOf(flags, "-mcpu=cortex-m85").? < indexOf(flags, "-mcpu=cortex-m33").?);
    try std.testing.expect(indexOf(flags, "-O0").? < indexOf(flags, "-Os").?);

    // And the inherited set survives: dropping it would change the image.
    try std.testing.expect(indexOf(flags, "-fdata-sections") != null);
    try std.testing.expect(indexOf(flags, "-std=gnu2x") != null);
    try std.testing.expect(indexOf(flags, "-DRA8_BUILD_FOR_CPU1") != null);

    // No first-party warning profile: those ride on ra8_add_app() targets, and
    // this executable is hand-rolled in the app's own CMakeLists.
    try std.testing.expect(indexOf(flags, "-Werror") == null);
    try std.testing.expect(indexOf(flags, "-Wstack-usage=2200") == null);
}

test "the second image's include path is the narrow one, not the app's" {
    const allocator = std.testing.allocator;
    const app = cpu1App(dual_core_app);
    const dirs = cpu1.includeDirs(allocator, app);
    defer allocator.free(dirs);
    defer for ([_]usize{ 0, 1, 4 }) |owned| allocator.free(dirs[owned]);

    try std.testing.expectEqual(@as(usize, 5), dirs.len);
    try std.testing.expectEqualStrings(
        "examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong/inc",
        dirs[0],
    );
    try std.testing.expectEqualStrings("libs/ra8_board_ek_ra8d2/inc", dirs[4]);

    // Only freestanding-clean headers may be reached from a Cortex-M33 TU, so
    // the four library directories the M85 image carries are absent here.
    for ([_][]const u8{
        "libs/ra8_net_pal/inc",
        "libs/ra8_usb_pal/inc",
        "libs/ra8_nsc/inc",
        "libs/ra8_secure_app/inc",
    }) |absent| {
        try std.testing.expect(indexOf(dirs, absent) == null);
    }
}

test "the replaced vendored unit is matched whole, not by prefix" {
    // cmake/threadx.cmake drops exactly one upstream unit, because the project
    // ships its own copy of it. Three files in that directory share the
    // `tx_initialize_` prefix, and a prefix match would silently delete the
    // kernel's entry and setup units along with it.
    try std.testing.expect(mw.isReplaced(mw.threadx, "tx_initialize_low_level.S"));
    try std.testing.expect(!mw.isReplaced(mw.threadx, "tx_initialize_kernel_enter.c"));
    try std.testing.expect(!mw.isReplaced(mw.threadx, "tx_initialize_kernel_setup.c"));
    // And the project copy that replaces it is compiled in its place, so the
    // symbol is defined exactly once.
    try std.testing.expect(indexOf(
        mw.threadx.project_sources,
        "port/threadx/src/cortex_m85/tx_initialize_low_level.S",
    ) != null);
}

test "a middleware exports defines, include dirs and link options onto its app" {
    // An arena: these builders hand back the ArrayList's own slice, whose
    // capacity need not equal its length, so the set is freed in one go
    // rather than slice by slice.
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const uses = mw.resolve(allocator, middleware_app.uses);
    const none = mw.resolve(allocator, bare_app.uses);

    // Each export is a separate mechanism, and three of the four fail
    // SILENTLY when they are missing: the kernel reads its own defaults
    // instead of port/threadx/inc/tx_user.h, the vendor headers land on the
    // app's -Werror include path, and the link succeeds with a time base that
    // never advances (issue #8).
    const defines = mw.appDefines(allocator, uses);
    try std.testing.expect(indexOf(defines, "-DTX_INCLUDE_USER_DEFINE_FILE") != null);

    const include_dirs = mw.appIncludeDirs(allocator, uses);
    try std.testing.expect(indexOf(include_dirs, "port/threadx/inc") != null);

    const system_dirs = mw.appSystemIncludeDirs(allocator, uses);
    try std.testing.expect(indexOf(system_dirs, "libs/third_party/threadx/common/inc") != null);
    // The vendor headers are -isystem, never -I: on the ordinary include path
    // their own diagnostics would fail the app's compile, not the vendor's.
    try std.testing.expect(indexOf(include_dirs, "libs/third_party/threadx/common/inc") == null);

    const link_options = mw.appLinkOptions(allocator, uses);
    try std.testing.expect(indexOf(link_options, "-Wl,--undefined=_tx_timer_interrupt") != null);

    // The other arm: an app that names no middleware gets none of it.
    try std.testing.expectEqual(@as(usize, 0), mw.appDefines(allocator, none).len);
    try std.testing.expectEqual(@as(usize, 0), mw.appIncludeDirs(allocator, none).len);
    try std.testing.expectEqual(@as(usize, 0), mw.appSystemIncludeDirs(allocator, none).len);
    try std.testing.expectEqual(@as(usize, 0), mw.appLinkOptions(allocator, none).len);
}

test "a middleware TU takes its own bar, and assembly is not the C bar" {
    const toolchain = mw.Toolchain{
        .gcc = "arm-none-eabi-gcc",
        .ar = "arm-none-eabi-ar",
        .global_defines = &.{"-DRA8_FREESTANDING"},
        .c_flags = &.{ "-mcpu=cortex-m85", "-fdata-sections", "-O0", "-g3", "-std=gnu2x" },
        .asm_flags = &.{ "-mcpu=cortex-m85", "-g3" },
    };

    const c_flags = mw.unitFlags(toolchain, mw.threadx, .{
        .path = "libs/third_party/threadx/common/src/tx_block_allocate.c",
        .language = .c,
    });
    const asm_flags = mw.unitFlags(toolchain, mw.threadx, .{
        .path = "libs/third_party/threadx/ports/cortex_m85/gnu/src/tx_thread_schedule.S",
        .language = .assembly,
    });

    // CMAKE_ASM_FLAGS is not CMAKE_C_FLAGS: the assembler is handed the CPU
    // selection and the debug level, and nothing else. Handing it the C set
    // is not a stricter build, it is a failed one.
    try std.testing.expect(indexOf(c_flags, "-std=gnu2x") != null);
    try std.testing.expect(indexOf(asm_flags, "-std=gnu2x") == null);
    try std.testing.expect(indexOf(asm_flags, "-fdata-sections") == null);
    try std.testing.expect(indexOf(asm_flags, "-mcpu=cortex-m85") != null);

    // Neither set carries a warning flag. The vendored sources have their
    // COMPILE_OPTIONS wiped, and the first-party SysTick glue compiled into
    // the same target never had the project profile either, so an app TU and
    // a middleware TU are held to genuinely different bars.
    for ([_][]const []const u8{ c_flags, asm_flags }) |set| {
        try std.testing.expect(indexOf(set, "-Werror") == null);
        try std.testing.expect(indexOf(set, "-Wall") == null);
        try std.testing.expect(indexOf(set, "-Wstack-usage=2200") == null);
    }
}

test "a middleware's include path is its own, not the app's" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const dirs = mw.includeDirs(arena.allocator(), mw.threadx);

    // Private first (tx_systick_retune.c reprograms SysTick from the live CGC
    // clock, so it needs the first-party headers), then the public one.
    try std.testing.expectEqualStrings("libs/ra8_core/inc", dirs[0]);
    try std.testing.expectEqualStrings("libs/ra8_hal/inc", dirs[1]);
    try std.testing.expectEqualStrings("port/threadx/inc", dirs[dirs.len - 1]);

    // No app directory, no board, no PAL: the middleware is built once and is
    // independent of which app links it.
    for ([_][]const u8{
        "libs/ra8_board_ek_ra8d2/inc",
        "libs/ra8_net_pal/inc",
        "libs/ra8_usb_pal/inc",
        "libs/ra8_secure_app/inc",
    }) |absent| {
        try std.testing.expect(indexOf(dirs, absent) == null);
    }
}

fn cpu1App(app: @TypeOf(dual_core_app)) cpu1.App {
    return .{ .name = app.name, .dir = app.dir, .board = app.board };
}

fn indexOf(haystack: []const []const u8, needle: []const u8) ?usize {
    for (haystack, 0..) |item, index| {
        if (std.mem.eql(u8, item, needle)) return index;
    }
    return null;
}
