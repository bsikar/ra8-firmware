//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What a cross configure hands each kind of translation unit, and what
//! TrustZone adds on top (#936, #1054, #1096).
//!
//! Extracted from build.zig because the root build file sits at the 1000-line
//! ceiling scripts/checks/check_file_size.py holds every Zig source to, and
//! this is the coherent piece: the FLAG sets, as opposed to the source-set
//! rules in cross_sources.zig and the step wiring that stays in build.zig.
//! Every set in here was measured from a real configure's own
//! compile_commands.json rather than read off a listfile, because CMake
//! composes a command line out of four scopes (toolchain file, directory,
//! target, source) and the listfile shows you only one of them at a time.

const std = @import("std");
const CrossApp = @import("cross_sources.zig").CrossApp;

/// The global CMAKE_C_FLAGS every translation unit in a cross configure
/// inherits, app target or not. The M85 app adds the dialect and warning sets
/// on top; the hand-rolled CPU1 target adds only its own options, which is why
/// this set has to be named separately rather than folded into the app bar.
pub const global_flags = cpu_flags ++ debug_flags ++ [_][]const u8{"-std=gnu2x"};

/// CPU flags from cmake/toolchain-ra8d2.cmake. The RA8D2 primary M85 is
/// single-precision, hence fpv5-sp-d16 with a hard float ABI; -mthumb because
/// the M-profile cores are Thumb-only. These go on compile AND link: the link
/// step picks its multilib from them.
pub const cpu_flags = cpu_select_flags ++ [_][]const u8{
    "-fdata-sections",
    "-ffunction-sections",
};

/// The CPU selection on its own. CMAKE_ASM_FLAGS carries only this and the
/// configuration's `-g3`: the assembler is handed no section splitting, no
/// optimisation level and no dialect, so a middleware's hand-written port
/// assembly cannot be given the C bar. Measured from a real configure's own
/// database, where all 14 assembly units differ from the C units in exactly
/// these flags.
pub const cpu_select_flags = [_][]const u8{
    "-mcpu=cortex-m85",
    "-mthumb",
    "-mfloat-abi=hard",
    "-mfpu=fpv5-sp-d16",
};

/// CMAKE_ASM_FLAGS plus CMAKE_ASM_FLAGS_DEBUG.
pub const asm_flags = cpu_select_flags ++ [_][]const u8{"-g3"};

/// Definitions cmake/toolchain-ra8d2.cmake adds at directory scope, so they
/// reach every target in a cross configure and not just the app. The app's own
/// bar repeats RA8_FREESTANDING through dialect_flags, which is where it
/// was first spelled (#936); this is the same define reaching a target that
/// has no first-party profile at all.
pub const global_defines = [_][]const u8{"-DRA8_FREESTANDING"};

/// The Debug configuration ra8_add_app() sets for a standalone app build.
pub const debug_flags = [_][]const u8{ "-O0", "-g3", "-DDEBUG" };

/// The dialect half CMake puts in CMAKE_C_FLAGS, so it reaches every target in
/// a cross configure and lands AHEAD of the app's warning profile.
pub const dialect_flags = [_][]const u8{
    "-std=gnu2x",
    "-DRA8_FREESTANDING",
};

/// The bare-metal half, which ra8_add_app() sets as target options and which
/// therefore lands AFTER the warning profile on the real compile line.
/// Position is the only thing that changed here (#1084): both flags are
/// order-insensitive against a -W list, but this list is also what
/// `zig build compile-db` writes, and a row whose argv is a permutation of the
/// compiler's is a row no consumer can diff against a real configure's.
/// -ffreestanding is what lets a firmware entry point be `void main(void)`;
/// drop it and every app main.c stops compiling.
pub const target_dialect_flags = [_][]const u8{
    "-ffreestanding",
    "-fshort-enums",
};

/// The first-party warning profile from cmake/ra8_warnings.cmake at this app's
/// STACK_BYTES budget. -Werror stays on for the same reason it does on the host
/// slice: a TU that only compiles here under a looser bar than CMake holds it to
/// would make the parity claim meaningless.
pub const warning_flags = [_][]const u8{
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wconversion",
    "-Wcast-qual",
    "-Wcast-align",
    "-Wdouble-promotion",
    "-Wformat=2",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wundef",
    "-Wvla",
    "-Wwrite-strings",
    "-Wbad-function-cast",
    "-Wmissing-declarations",
    "-Wmissing-prototypes",
    "-Wnested-externs",
    "-Wold-style-definition",
    "-Wredundant-decls",
    "-Wstrict-prototypes",
    "-Wduplicated-branches",
    "-Wduplicated-cond",
    "-Wformat-overflow=2",
    "-Wformat-truncation=2",
    "-Wlogical-op",
};

/// The two stack-budget flags, spelled at THIS app's budget. They are not part
/// of the list above because the budget is per-app data, not a constant: see
/// CrossApp.stack_bytes. -fstack-usage rides along with the gate because the
/// same call in cmake/ra8_warnings.cmake adds both, and the `.su` files it
/// writes are what scripts/checks/stack_usage_check.py aggregates.
pub fn warningFlags(allocator: std.mem.Allocator, app: CrossApp) []const []const u8 {
    return warningFlagsForStack(allocator, app.stack_bytes);
}

/// The same profile at a budget given directly, for a target that is not an
/// app: the Non-Secure image of a two-project TrustZone build is a raw
/// add_executable() that calls ra8_target_enable_project_warnings() itself
/// (#1111), so it has a frame budget without being a CrossApp.
pub fn warningFlagsForStack(allocator: std.mem.Allocator, stack_bytes: u32) []const []const u8 {
    var flags = std.ArrayList([]const u8).init(allocator);
    flags.appendSlice(&warning_flags) catch @panic("OOM");
    const gate = std.fmt.allocPrint(allocator, "-Wstack-usage={d}", .{stack_bytes}) catch @panic("OOM");
    flags.append(gate) catch @panic("OOM");
    flags.append("-fstack-usage") catch @panic("OOM");
    return flags.toOwnedSlice() catch @panic("OOM");
}

/// Link flags from the toolchain file: no hosted runtime, prune unused
/// sections, and report the region usage the map file details.
pub const link_flags = [_][]const u8{
    "-nostdlib",
    "-Wl,--gc-sections",
    "-Wl,--print-memory-usage",
};

/// What `RA8_TRUSTZONE_ENABLE` adds, and where.
///
/// The option is OFF at the repo root, so the six apps this graph cross-built
/// before #1096 never saw either flag. It is ON for a standalone configure of
/// a TrustZone app, because that app's own CMakeLists declares the option
/// `ON` when it is the top-level listfile, and a standalone configure is what
/// this graph reproduces.
///
/// `define` lands with the directory-scope defines, immediately after
/// RA8_FREESTANDING, and gates the SAU programming and the secure-side entry
/// points in first-party headers.
///
/// `cmse` is the sharp one. It is what makes GCC emit a Secure-Gateway veneer
/// into `.gnu.sgstubs` for every `__attribute__((cmse_nonsecure_entry))`
/// function, and it goes on BOTH the compile and the link. Nothing fails
/// without it: the same sources compile, the same image links, and the
/// veneers the Non-Secure world calls through simply do not exist. It is
/// emitted LAST on the compile line, after the target dialect flags, because
/// ra8_add_app() adds it as a target option after those.
pub const trust_zone = struct {
    pub const define = "-DRA8_TRUSTZONE_ENABLE";
    pub const cmse = "-mcmse";
    /// The linker half of the CMSE contract, added by the app's own
    /// CMakeLists rather than by ra8_add_app(). `--out-implib` writes the
    /// import library the Non-Secure link binds veneer names against, so the
    /// NS image reaches each veneer by name at its `.gnu.sgstubs` address.
    /// Without it the secure ELF is identical in every other respect and the
    /// NS half of a two-project build cannot be linked at all.
    pub const implib_flag = "-Wl,--cmse-implib";
    pub const out_implib_prefix = "-Wl,--out-implib=";
};

test "trustzone flags are the two GCC spells them, not an approximation" {
    try std.testing.expectEqualStrings("-DRA8_TRUSTZONE_ENABLE", trust_zone.define);
    try std.testing.expectEqualStrings("-mcmse", trust_zone.cmse);
}
