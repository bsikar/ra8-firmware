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
const local = graph.app_local;
const ns = graph.ns_image;
const arm_flags = graph.arm_flags;

/// The apps the cross slice builds, by the rules they exercise: one that names
/// no libraries at all, one that names two, and one that keeps more than a
/// single translation unit under its own `src/`.
const bare_app = graph.cross_apps[0];
const library_app = graph.cross_apps[1];
const dual_core_app = graph.cross_apps[2];
/// And one that names a vendored middleware in `USES`, which is the only way
/// the middleware exports below are observable at all.
const middleware_app = graph.cross_apps[3];
/// And one that names a non-default `STACK_BYTES` budget, without which the
/// frame gate below reads as a constant that happens to be right.
const deep_stack_app = graph.cross_apps[4];
/// And the one whose own CMakeLists does work beyond ra8_add_app(): five
/// EXTRA_SRCS helpers and a vendored static library it declares and links.
const extra_srcs_app = graph.cross_apps[5];
/// And the first TrustZone one, which is the only app in the tree naming
/// NSC_SRCS and the only one whose standalone configure has
/// RA8_TRUSTZONE_ENABLE ON.
const trust_zone_app = graph.cross_apps[6];
/// And the one that names OFF_TARGET_LIBS, the only app in the table whose own
/// translation units are not all compiled at the same preprocessor view.
const off_target_app = graph.cross_apps[8];
/// And the one that names NO_NSC, the only app in the tree that compiles none
/// of libs/ra8_nsc/src, and the only one that is dual-core and TrustZone at
/// once.
const no_nsc_app = graph.cross_apps[9];

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

test "the frame gate is spelled at the app's own budget, not at one constant" {
    const allocator = std.testing.allocator;

    const bare = graph.armWarningFlags(allocator, bare_app);
    defer allocator.free(bare);
    defer for (bare) |flag| {
        if (std.mem.startsWith(u8, flag, "-Wstack-usage=")) allocator.free(flag);
    };
    const deep = graph.armWarningFlags(allocator, deep_stack_app);
    defer allocator.free(deep);
    defer for (deep) |flag| {
        if (std.mem.startsWith(u8, flag, "-Wstack-usage=")) allocator.free(flag);
    };

    // Both arms of the rule, which is the whole point of the fifth app: the
    // default budget and a named one, from the same function.
    try std.testing.expectEqual(@as(u32, 2200), bare_app.stack_bytes);
    try std.testing.expectEqual(@as(u32, 4096), deep_stack_app.stack_bytes);
    try std.testing.expect(indexOf(bare, "-Wstack-usage=2200") != null);
    try std.testing.expect(indexOf(deep, "-Wstack-usage=4096") != null);
    try std.testing.expect(indexOf(deep, "-Wstack-usage=2200") == null);

    // -fstack-usage rides along with the gate, because the one call in
    // cmake/ra8_warnings.cmake adds both and stack_usage_check.py reads the
    // `.su` files it writes.
    try std.testing.expect(indexOf(bare, "-fstack-usage") != null);
    try std.testing.expect(indexOf(deep, "-fstack-usage") != null);

    // And the rest of the profile is identical between the two apps: the
    // budget is the ONLY thing an app's STACK_BYTES changes.
    try std.testing.expectEqual(bare.len, deep.len);
    for (bare, deep) |left, right| {
        if (std.mem.startsWith(u8, left, "-Wstack-usage=")) continue;
        try std.testing.expectEqualStrings(left, right);
    }
    try std.testing.expect(indexOf(deep, "-Werror") != null);
}

test "the console-stream gate opens only for the app that names ra8_io" {
    const console = "libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_console_stream.c";

    // The arm no app in the table had taken before ra8_io_swap_demo: the unit
    // hands back an ra8_io_stream_t, so naming the full ra8_io keeps it.
    try std.testing.expect(sources.declaresLibrary(deep_stack_app, "ra8_io"));
    try std.testing.expect(!sources.isGatedOutBoardSource(deep_stack_app, console));

    // The bus facade alone does NOT satisfy it, which is what makes this a
    // second gate rather than a rewording of the touch one.
    try std.testing.expect(sources.isGatedOutBoardSource(library_app, console));
    try std.testing.expect(sources.isGatedOutBoardSource(bare_app, console));

    // The touch unit is satisfied by either library, so it is kept for both.
    const touch = "libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_touch.c";
    try std.testing.expect(!sources.isGatedOutBoardSource(deep_stack_app, touch));
    try std.testing.expect(!sources.isGatedOutBoardSource(library_app, touch));
}

test "a gated library unit is dropped unless its companion library is named" {
    const vsource = "libs/ra8_io/src/ra8_io_blockdev_vsource.c";

    // ra8_io globs it in; ra8_mem is what keeps it. ra8_io_swap_demo names
    // the first and not the second, which is the arm CMake was measured on.
    try std.testing.expect(sources.declaresLibrary(deep_stack_app, "ra8_io"));
    try std.testing.expect(!sources.declaresLibrary(deep_stack_app, "ra8_mem"));
    try std.testing.expect(sources.isGatedOutLibrarySource(deep_stack_app, vsource));

    // Every other unit out of the same directory stays.
    try std.testing.expect(
        !sources.isGatedOutLibrarySource(deep_stack_app, "libs/ra8_io/src/ra8_io_stream.c"),
    );

    // And the gate is keyed on the whole path, not on a name fragment: an
    // app-local file that merely ends the same way is untouched.
    try std.testing.expect(
        !sources.isGatedOutLibrarySource(deep_stack_app, "libs/ra8_mem/src/ra8_io_blockdev_vsource.c"),
    );
}

fn indexOf(haystack: []const []const u8, needle: []const u8) ?usize {
    for (haystack, 0..) |item, index| {
        if (std.mem.eql(u8, item, needle)) return index;
    }
    return null;
}

test "EXTRA_SRCS helpers are compiled in, in the order the app names them" {
    // Five helpers out of two libraries the app does NOT name in LIBS. The
    // graph has to take them from the app's own declaration; nothing about
    // libs/ra8_dfu/src says four of its units belong to this image and the
    // rest do not.
    const expected = [_][]const u8{
        "libs/ra8_psa_crypto/src/ra8_psa_crypto.c",
        "libs/ra8_dfu/src/ra8_rot.c",
        "libs/ra8_dfu/src/ra8_dfu_antirollback.c",
        "libs/ra8_dfu/src/ra8_dfu_boot.c",
        "libs/ra8_dfu/src/ra8_dfu_launch.c",
    };
    try std.testing.expectEqual(expected.len, extra_srcs_app.extra_srcs.len);
    for (expected, extra_srcs_app.extra_srcs) |want, got| {
        try std.testing.expectEqualStrings(want, got);
    }

    // And the rest of libs/ra8_dfu/src stays out: naming five units is not
    // naming the library.
    try std.testing.expect(!sources.declaresLibrary(extra_srcs_app, "ra8_dfu"));
    try std.testing.expect(!sources.declaresLibrary(extra_srcs_app, "ra8_psa_crypto"));

    // No other app in the table names any, so the keyword was dead code in the
    // graph until this app arrived.
    try std.testing.expectEqual(@as(usize, 0), bare_app.extra_srcs.len);
    try std.testing.expectEqual(@as(usize, 0), middleware_app.extra_srcs.len);
}

test "an app-local vendored library exports its defines and system dirs onto the app" {
    const app_defines = local.appDefines(std.testing.allocator, extra_srcs_app.local);
    defer std.testing.allocator.free(app_defines);

    // The library's PUBLIC set first, then the app target's own PRIVATE one,
    // the order a real configure's database shows. Missing the first four is
    // silent: the app is then preprocessed against a different crypto
    // configuration than the archive it links.
    try std.testing.expectEqual(@as(usize, 5), app_defines.len);
    try std.testing.expectEqualStrings("-DMBEDTLS_CONFIG_FILE=\"mbedtls_config.h\"", app_defines[0]);
    try std.testing.expectEqualStrings("-DRA8_ENABLE_ROOT_OF_TRUST", app_defines[4]);

    // SYSTEM, not ordinary: on the app's -Werror -Wconversion bar the vendor
    // headers would fail the app's own compile.
    const system_dirs = local.appSystemIncludeDirs(extra_srcs_app.local);
    try std.testing.expectEqual(@as(usize, 10), system_dirs.len);
    try std.testing.expectEqualStrings("libs/third_party/tf-psa-crypto/include", system_dirs[0]);
    try std.testing.expectEqualStrings("port/mbedtls/inc", system_dirs[9]);

    // An app with no CMakeLists of its own takes neither.
    const none = local.appDefines(std.testing.allocator, bare_app.local);
    defer std.testing.allocator.free(none);
    try std.testing.expectEqual(@as(usize, 0), none.len);
    try std.testing.expectEqual(@as(usize, 0), local.appSystemIncludeDirs(bare_app.local).len);
}

test "a vendored library declared by the app carries no project warning profile" {
    const lib = extra_srcs_app.local.vendored.?;
    const flags = local.compileFlags(std.testing.allocator, .{
        .gcc = "arm-none-eabi-gcc",
        .ar = "arm-none-eabi-ar",
        .global_flags = &.{ "-mcpu=cortex-m85", "-O0", "-g3", "-DDEBUG", "-std=gnu2x" },
        .global_defines = &.{"-DRA8_FREESTANDING"},
    }, lib);
    defer std.testing.allocator.free(flags);

    // ra8_target_enable_project_warnings() is applied by ra8_add_app() to the
    // APP target; this library never passed through it. Compiling 77 TUs of
    // vendored crypto at the first-party bar does not build.
    for (flags) |flag| {
        try std.testing.expect(!std.mem.startsWith(u8, flag, "-W"));
        try std.testing.expect(!std.mem.startsWith(u8, flag, "-fstack-usage"));
    }
    // The one option it does carry changes code generation, not diagnostics.
    try std.testing.expect(indexOf(flags, "-fno-strict-aliasing") != null);
    // And the directory-scope define reaches it even so.
    try std.testing.expect(indexOf(flags, "-DRA8_FREESTANDING") != null);
    // Its own PUBLIC defines are on its own TUs too, not only on the app's.
    try std.testing.expect(indexOf(flags, "-DMBEDTLS_PLATFORM_MEMORY") != null);
}

test "NSC_SRCS compiles the named veneer subset and drops the rest" {
    // The app names exactly one of the ten translation units in
    // libs/ra8_nsc/src. The other nine do not compile under its secure
    // configuration, and a glob puts them in an image CMake never put them in.
    try std.testing.expect(sources.isGatedOutNscSource(trust_zone_app, "libs/ra8_nsc/src/ra8_nsc_comms.c"));
    try std.testing.expect(sources.isGatedOutNscSource(trust_zone_app, "libs/ra8_nsc/src/ra8_nsc_eth.c"));
    try std.testing.expect(!sources.isGatedOutNscSource(trust_zone_app, "libs/ra8_nsc/src/ra8_nsc_cgc.c"));

    // The other arm: an app naming no NSC_SRCS gets the whole directory, so
    // the rule is a narrowing and not a filter every app pays.
    try std.testing.expect(!sources.isGatedOutNscSource(bare_app, "libs/ra8_nsc/src/ra8_nsc_comms.c"));
    try std.testing.expect(!sources.isGatedOutNscSource(bare_app, "libs/ra8_nsc/src/ra8_nsc_cgc.c"));

    // And it is scoped to that one directory: a same-named file elsewhere is
    // not swept up by an app that narrowed the NSC set.
    try std.testing.expect(!sources.isGatedOutNscSource(trust_zone_app, "libs/ra8_core/src/ra8_nsc_comms.c"));
}

test "TrustZone is the two flags, on the app that declares it and no other" {
    try std.testing.expect(trust_zone_app.trust_zone);
    try std.testing.expect(!bare_app.trust_zone);
    try std.testing.expect(!extra_srcs_app.trust_zone);

    // -mcmse is what emits the Secure-Gateway veneers. Nothing fails without
    // it: the same sources compile and the same image links, with an empty
    // .gnu.sgstubs for the Non-Secure world to call into.
    try std.testing.expectEqualStrings("-mcmse", graph.arm_flags.trust_zone.cmse);
    try std.testing.expectEqualStrings("-DRA8_TRUSTZONE_ENABLE", graph.arm_flags.trust_zone.define);

    // Neither flag is in a set every app gets: they are conditional on the
    // app, which is what the six earlier apps prove by not having them.
    for (graph.arm_flags.global_flags) |flag| {
        try std.testing.expect(!std.mem.eql(u8, flag, "-mcmse"));
    }
    for (graph.arm_flags.target_dialect_flags) |flag| {
        try std.testing.expect(!std.mem.eql(u8, flag, "-mcmse"));
    }
}

test "the secure half names the CMSE import library, and only it does" {
    // The import library is what the Non-Secure link binds veneer names
    // against, so it is an output of the secure link rather than a by-product.
    try std.testing.expectEqualStrings("tz_nsc_cgc_usb_cmse_import.o", trust_zone_app.cmse_implib.?);
    try std.testing.expect(bare_app.cmse_implib == null);
    try std.testing.expect(dual_core_app.cmse_implib == null);

    // The three ns_*.c files under the app's own src/ belong to the SEPARATE
    // Non-Secure executable. They are the app-local glob's sharpest case
    // here: compiled into the secure image they would be a second world's
    // code inside the secure one, and the link would not complain.
    try std.testing.expect(sources.isAuxSource(trust_zone_app, "src/ns_main.c"));
    try std.testing.expect(sources.isAuxSource(trust_zone_app, "src/ns_usb.c"));
    try std.testing.expect(!sources.appLocalIsCompiled(trust_zone_app, "src/ns_usb_host.c"));
    // While its own two boot overrides are compiled from the app's copy, not
    // the board's. First app in the table to override more than one.
    for ([_][]const u8{ "system_init.c", "trustzone_init.c" }) |boot| {
        const own = sources.bootSourcePath(std.testing.allocator, trust_zone_app, boot, true);
        defer std.testing.allocator.free(own);
        const expected = try std.fmt.allocPrint(
            std.testing.allocator,
            "examples/ek_ra8d2/hil_needs_revalidation/tz_nsc_cgc_usb/src/{s}",
            .{boot},
        );
        defer std.testing.allocator.free(expected);
        try std.testing.expectEqualStrings(expected, own);
    }
}

/// The Non-Secure image of the two-project TrustZone app (#1111). Its rules
/// live on the app entry above, which is the only one in the tree that has a
/// second, separate executable rather than an embedded blob.
const ns_image_of = trust_zone_app.ns.?;

test "the NS image's vendored globs take both arms of every filter" {
    // The two simulators are excluded by the app's own list(FILTER EXCLUDE
    // REGEX), and the exclusion is a PREFIX on the basename: the neighbouring
    // real device-stack sources stay.
    const core = ns_image_of.vendored[0];
    try std.testing.expect(!ns.vendoredSelects(core, "ux_dcd_sim_slave_initialize.c"));
    try std.testing.expect(!ns.vendoredSelects(core, "ux_hcd_sim_host_entry.c"));
    try std.testing.expect(ns.vendoredSelects(core, "ux_device_stack_initialize.c"));
    try std.testing.expect(ns.vendoredSelects(core, "ux_utility_memory_allocate.c"));
    // Headers are never sources, whatever the prefix says.
    try std.testing.expect(!ns.vendoredSelects(core, "ux_device_stack_initialize.h"));

    // The class tree holds 225 drivers and this image wants the CDC-ACM ones,
    // so the glob is a prefix rather than the directory.
    const cdc = ns_image_of.vendored[1];
    try std.testing.expect(ns.vendoredSelects(cdc, "ux_device_class_cdc_acm_read.c"));
    try std.testing.expect(!ns.vendoredSelects(cdc, "ux_device_class_storage_read.c"));

    // And the first-party bridge glob is ux_dcd_ra8_usb*, which is why the
    // host-controller driver and the storage class TU sitting in the same
    // directory do not join the NS image.
    const bridge = ns_image_of.vendored[2];
    try std.testing.expect(ns.vendoredSelects(bridge, "ux_dcd_ra8_usb_ep.c"));
    try std.testing.expect(!ns.vendoredSelects(bridge, "ux_hcd_ra8_usb.c"));
    try std.testing.expect(!ns.vendoredSelects(bridge, "ux_device_class_storage_inquiry.c"));
}

test "the NS suppressions are on the vendored sets and not on the bridge" {
    try std.testing.expectEqual(@as(usize, 2), ns_image_of.vendored[0].suppressions.len);
    try std.testing.expectEqualStrings("-Wno-discarded-qualifiers", ns_image_of.vendored[0].suppressions[0]);
    try std.testing.expectEqualStrings("-Wno-cast-align", ns_image_of.vendored[0].suppressions[1]);
    try std.testing.expectEqual(@as(usize, 2), ns_image_of.vendored[1].suppressions.len);
    // The bridge is first-party: it keeps -Wcast-align, which is one of the
    // two the vendor sources are excused from.
    try std.testing.expectEqual(@as(usize, 0), ns_image_of.vendored[2].suppressions.len);

    // A suppression rides at the END of the unit's flag list, after the
    // warning profile it is excusing, or it suppresses nothing.
    const warnings = [_][]const u8{ "-Wall", "-Werror", "-Wcast-align" };
    // These helpers hand back an ArrayList's items, whose capacity need not
    // equal its length, so the test owns an arena rather than freeing slices.
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const flags = ns.compileFlags(
        arena.allocator(),
        &.{"-std=gnu2x"},
        &warnings,
        .{ .path = "x.c", .suppressions = ns_image_of.vendored[0].suppressions },
    );
    try std.testing.expectEqualStrings("-Wno-cast-align", flags[flags.len - 1]);
    // And the raw target's dialect options land BEFORE the profile here, the
    // reverse of what ra8_add_app() produces: this app's CMakeLists calls
    // target_compile_options() before ra8_target_enable_project_warnings().
    try std.testing.expectEqualStrings("-fshort-enums", flags[1]);
    try std.testing.expectEqualStrings("-ffreestanding", flags[2]);
    try std.testing.expectEqualStrings("-Wall", flags[3]);
}

test "the NS image's defines are sorted and deduplicated, not declaration order" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const flags = ns.defines(
        arena.allocator(),
        ns_image_of,
        mw.threadx_ns,
        &.{"-DRA8_FREESTANDING"},
    );
    // The target declares RA8_TRUSTZONE_ENABLE first, yet CMake's generator
    // keeps definitions in a set and emits them lexicographically. A graph
    // that wrote declaration order would write database rows no consumer
    // could diff against a real configure.
    const expected = [_][]const u8{
        "-DRA8_FREESTANDING",
        "-DRA8_PERIPH_NS_ALIAS",
        "-DRA8_THREADX_NON_SECURE",
        "-DRA8_TRUSTZONE_ENABLE",
        "-DRA8_USB_POLLED_ONLY",
        "-DTX_INCLUDE_USER_DEFINE_FILE",
    };
    try std.testing.expectEqual(expected.len, flags.len);
    for (expected, flags) |want, got| try std.testing.expectEqualStrings(want, got);
}

test "threadx_ns is a different archive from threadx, not the same one with a flag" {
    // The define that flips tx_user.h from TX_SINGLE_MODE_SECURE to
    // TX_SINGLE_MODE_NON_SECURE. Same kernel sources, different kernel.
    var secure_has_ns_define = false;
    for (mw.threadx.public_defines) |define| {
        if (std.mem.eql(u8, define, "-DRA8_THREADX_NON_SECURE")) secure_has_ns_define = true;
    }
    try std.testing.expect(!secure_has_ns_define);
    try std.testing.expectEqualStrings("-DRA8_THREADX_NON_SECURE", mw.threadx_ns.public_defines[0]);

    // The NS archive drops the SysTick retune (it reprograms a secure-world
    // peripheral) and carries the three freestanding shims instead, because
    // the NS image links no libc and no libgcc at all.
    var secure_has_retune = false;
    for (mw.threadx.project_sources) |source| {
        if (std.mem.endsWith(u8, source, "tx_systick_retune.c")) secure_has_retune = true;
    }
    try std.testing.expect(secure_has_retune);
    var ns_shims: usize = 0;
    for (mw.threadx_ns.project_sources) |source| {
        try std.testing.expect(!std.mem.endsWith(u8, source, "tx_systick_retune.c"));
        if (std.mem.indexOf(u8, source, "ra8_freestanding_") != null) ns_shims += 1;
    }
    try std.testing.expectEqual(@as(usize, 3), ns_shims);

    // Dropping that TU is why the private include path narrows: ra8_hal was
    // only ever there for it.
    try std.testing.expectEqual(@as(usize, 1), mw.threadx_ns.private_include_dirs.len);
    try std.testing.expectEqualStrings("libs/ra8_core/inc", mw.threadx_ns.private_include_dirs[0]);

    // And the -I order is each listfile's own call order, not a convention:
    // threadx_ns declares PUBLIC first, threadx declares PRIVATE first.
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const ns_dirs = mw.includeDirs(arena.allocator(), mw.threadx_ns);
    try std.testing.expectEqualStrings("port/threadx/inc", ns_dirs[0]);
    try std.testing.expectEqualStrings("libs/ra8_core/inc", ns_dirs[1]);
    const secure_dirs = mw.includeDirs(arena.allocator(), mw.threadx);
    try std.testing.expectEqualStrings("libs/ra8_core/inc", secure_dirs[0]);
    try std.testing.expectEqualStrings("port/threadx/inc", secure_dirs[secure_dirs.len - 1]);
}

test "the NS image's own sources are exactly the AUX_SRCS the secure image excludes" {
    // One file set, two images. The only thing keeping ns_main.c out of the
    // M85 secure image and inside the NS one is these two lists agreeing.
    try std.testing.expectEqual(trust_zone_app.aux_srcs.len, ns_image_of.app_sources.len);
    for (trust_zone_app.aux_srcs, ns_image_of.app_sources) |aux, own| {
        try std.testing.expectEqualStrings(aux, own);
    }
    // The NS link has no -lgcc, so its libc comes from the archive: assert the
    // image really does name the NS middleware variant rather than the secure
    // one, which does not carry the shims.
    try std.testing.expectEqualStrings("threadx_ns", ns_image_of.uses);
    try std.testing.expectEqualStrings("ns_image.ld", ns_image_of.linker_script);
    try std.testing.expectEqualStrings("-nostartfiles", ns_image_of.link_flags[0]);
}
test "OFF_TARGET_LIBS carries the define on its own units and on nothing else" {
    // Both arms, because the predicate is the only thing separating CMake's
    // image from a wrong one. On this app the wrong arm fails closed (the
    // library's on-target half includes psa/crypto.h, which this app's
    // include path does not carry); on an app that carries the TF-PSA headers
    // anyway both halves compile and the difference is silent. See
    // cross_sources.off_target_define for the measurement of both.
    try std.testing.expectEqual(@as(usize, 1), off_target_app.off_target_libs.len);
    try std.testing.expectEqualStrings("ra8_psa_crypto", off_target_app.off_target_libs[0]);
    try std.testing.expectEqualStrings("-DRA8_OFF_TARGET", sources.off_target_define);

    try std.testing.expect(sources.isOffTargetSource(
        off_target_app,
        "libs/ra8_psa_crypto/src/ra8_psa_crypto.c",
    ));
    try std.testing.expect(sources.isOffTargetSource(
        off_target_app,
        "libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c",
    ));
    // Its own main.c, a universal unit and a unit of the board it names in
    // LIBS all stay at the ordinary bar.
    try std.testing.expect(!sources.isOffTargetSource(
        off_target_app,
        "examples/ek_ra8d2/hw_validated/hil/crypto_aes_demo/src/main.c",
    ));
    try std.testing.expect(!sources.isOffTargetSource(
        off_target_app,
        "libs/ra8_core/src/ra8_log.c",
    ));
    try std.testing.expect(!sources.isOffTargetSource(
        off_target_app,
        "libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_led.c",
    ));
    // And no other app in the table takes the define at all, which is what
    // makes the predicate a rule rather than a constant: the same path under
    // an app that does not name the library is an ordinary unit.
    for (graph.cross_apps) |app| {
        if (std.mem.eql(u8, app.name, off_target_app.name)) continue;
        try std.testing.expect(!sources.isOffTargetSource(
            app,
            "libs/ra8_psa_crypto/src/ra8_psa_crypto.c",
        ));
    }
}

test "an off-target library is a LIBS name only in how its units are compiled" {
    // The keyword is deliberately NOT spelled in LIBS: a library named there
    // would get the same sources and the same include directory and no define,
    // which is exactly the wrong image. Assert the two lists stay disjoint.
    for (off_target_app.off_target_libs) |off_target| {
        try std.testing.expect(!sources.declaresLibrary(off_target_app, off_target));
    }
    try std.testing.expectEqual(@as(usize, 1), off_target_app.libraries.len);
    try std.testing.expectEqualStrings("ra8_board_ek_ra8d2", off_target_app.libraries[0]);
    // It names no middleware, no EXTRA_SRCS, no second image and no vendored
    // library of its own, so everything this app's rows differ from
    // blink_hal's by IS the off-target keyword.
    try std.testing.expectEqual(@as(usize, 0), off_target_app.uses.len);
    try std.testing.expectEqual(@as(usize, 0), off_target_app.extra_srcs.len);
    try std.testing.expectEqual(@as(usize, 0), off_target_app.aux_srcs.len);
    try std.testing.expect(off_target_app.cpu1 == null);
    try std.testing.expect(off_target_app.ns == null);
    try std.testing.expect(off_target_app.local.vendored == null);
    try std.testing.expect(!off_target_app.trust_zone);
    try std.testing.expectEqual(@as(u32, 2200), off_target_app.stack_bytes);
}

test "the NSC set is one decision with three arms, and every app takes one" {
    // NO_NSC compiles none of the directory.
    try std.testing.expect(no_nsc_app.no_nsc);
    for ([_][]const u8{
        "libs/ra8_nsc/src/ra8_nsc_cgc.c",
        "libs/ra8_nsc/src/ra8_nsc_comms.c",
        "libs/ra8_nsc/src/ra8_nsc_xspi.c",
    }) |unit| {
        try std.testing.expect(!sources.nscIsCompiled(no_nsc_app, unit));
    }

    // NSC_SRCS compiles exactly the named subset: the same first unit the
    // app above drops is the one unit this app keeps.
    try std.testing.expect(sources.nscIsCompiled(trust_zone_app, "libs/ra8_nsc/src/ra8_nsc_cgc.c"));
    try std.testing.expect(!sources.nscIsCompiled(trust_zone_app, "libs/ra8_nsc/src/ra8_nsc_comms.c"));

    // And an app that names neither keyword compiles the whole directory,
    // which is the arm every app before #1096 takes.
    try std.testing.expect(!bare_app.no_nsc);
    try std.testing.expectEqual(@as(usize, 0), bare_app.nsc_srcs.len);
    for ([_][]const u8{
        "libs/ra8_nsc/src/ra8_nsc_cgc.c",
        "libs/ra8_nsc/src/ra8_nsc_comms.c",
        "libs/ra8_nsc/src/ra8_nsc_xspi.c",
    }) |unit| {
        try std.testing.expect(sources.nscIsCompiled(bare_app, unit));
    }

    // The rule answers about that one directory and nothing else: an app
    // compiling no veneers still compiles its own main.c and every universal
    // unit.
    try std.testing.expect(sources.nscIsCompiled(
        no_nsc_app,
        "examples/ek_ra8d2/hil_needs_revalidation/cpu1_pingpong_ipc/src/main.c",
    ));
    try std.testing.expect(sources.nscIsCompiled(no_nsc_app, "libs/ra8_core/src/ra8_log.c"));
    // And only one app in the table takes the NO_NSC arm, so the predicate is
    // a rule rather than a constant.
    var excluding: usize = 0;
    for (graph.cross_apps) |app| {
        if (app.no_nsc) excluding += 1;
    }
    try std.testing.expectEqual(@as(usize, 1), excluding);
}

test "excluding the NSC sources does not take their include directory off the path" {
    // The two halves of the keyword are not symmetrical, and this is the half
    // a listing cannot tell you: libs/ra8_nsc/inc is in the UNIVERSAL include
    // set, fed by its own list rather than by the source glob, so it stays on
    // the path of every unit in an app that compiles none of those sources.
    // The app's own headers still name the veneer prototypes it calls the
    // Non-Secure world through.
    try std.testing.expect(indexOf(&sources.cross_include_dirs, "libs/ra8_nsc/inc") != null);
    try std.testing.expect(indexOf(&sources.cross_source_dirs, "libs/ra8_nsc/src") != null);
}

test "the app that excludes the NSC set names nothing else that could explain its rows" {
    // Its first-party set is blink_hal's universal 200, minus the ten NSC
    // units, plus ra8_tz_secure_boot from LIBS, plus the third unit it keeps
    // under its own src/. So no middleware, no EXTRA_SRCS, no off-target
    // library, no vendored library of its own and no Non-Secure image: what
    // its rows differ from blink_hal's by is this keyword and the app-local
    // sources named beside it.
    try std.testing.expectEqual(@as(usize, 0), no_nsc_app.uses.len);
    try std.testing.expectEqual(@as(usize, 0), no_nsc_app.extra_srcs.len);
    try std.testing.expectEqual(@as(usize, 0), no_nsc_app.off_target_libs.len);
    try std.testing.expectEqual(@as(usize, 0), no_nsc_app.nsc_srcs.len);
    try std.testing.expect(no_nsc_app.local.vendored == null);
    try std.testing.expect(no_nsc_app.ns == null);
    try std.testing.expect(no_nsc_app.cmse_implib == null);
    try std.testing.expectEqual(@as(u32, 2200), no_nsc_app.stack_bytes);
    try std.testing.expectEqual(@as(usize, 1), no_nsc_app.libraries.len);
    try std.testing.expectEqualStrings("ra8_tz_secure_boot", no_nsc_app.libraries[0]);

    // AUX_SRCS names ONE file here, so src/ns_main.c belongs to the M85 image
    // despite its name: the arm of the app-local glob no other app takes.
    try std.testing.expectEqual(@as(usize, 1), no_nsc_app.aux_srcs.len);
    try std.testing.expect(sources.isAuxSource(no_nsc_app, "src/cpu1_main.c"));
    try std.testing.expect(!sources.appLocalIsCompiled(no_nsc_app, "src/cpu1_main.c"));
    try std.testing.expect(sources.appLocalIsCompiled(no_nsc_app, "src/ns_main.c"));
    // And its two boot overrides resolve to its own copies, not the board's.
    for ([_][]const u8{ "system_init.c", "trustzone_init.c" }) |boot| {
        const resolved = sources.bootSourcePath(std.testing.allocator, no_nsc_app, boot, true);
        defer std.testing.allocator.free(resolved);
        try std.testing.expect(std.mem.startsWith(u8, resolved, no_nsc_app.dir));
    }
}

// The second-image (Cortex-M33) rules live in their own file: this one hit the
// 1000-line ceiling scripts/checks/check_file_size.py holds every Zig source
// to, and the CPU1 tests are the coherent piece to lift out. Same module, so
// they still reach the graph through the `build_graph` import (#1146).
test {
    _ = @import("cpu1_image_test.zig");
    _ = @import("analysis_test.zig");
    _ = @import("command_surface_test.zig");
}
