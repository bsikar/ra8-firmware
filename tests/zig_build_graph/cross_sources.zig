//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which translation units an example app compiles, and on what include path
//! (#936, widened by #1017).
//!
//! Extracted from build.zig because the root build file is at the 1000-line
//! ceiling scripts/checks/check_file_size.py holds every Zig source to, and
//! this is the coherent piece: the SOURCE-SET rules ra8_add_app() applies, as
//! opposed to the flags, the tool discovery and the step wiring that stay
//! beside the rest of the graph.
//!
//! Every rule in here is one a directory listing cannot tell you, which is why
//! it is data with tests rather than a glob: board boot files resolve per app,
//! two board units are opt-in on a library the app must name, one library name
//! has no directory of its own at all, and one app-local source under `src/`
//! belongs to a second image and must be kept OUT of this one (#1029).

const std = @import("std");
const cpu1_image = @import("cpu1_image.zig");
const app_local_mod = @import("app_local.zig");
const ns_image_mod = @import("ns_image.zig");

/// The app this slice cross-builds, spelled the way ra8_add_app() resolves it.
pub const CrossApp = struct {
    name: []const u8,
    dir: []const u8,
    board: []const u8,
    linker_script: []const u8,
    /// Everything the app names in `LIBS`, migrated or not. Read by the
    /// board opt-in gate below, which keys off the declared set rather than
    /// off what happens to be on disk.
    libraries: []const []const u8,
    zig_libraries: []const []const u8,
    /// Translation units under the app's own `src/` that belong to a DIFFERENT
    /// image and must stay out of this one, spelled relative to the app
    /// directory exactly as `AUX_SRCS` spells them. See aux_srcs below.
    aux_srcs: []const []const u8 = &.{},
    /// The second (Cortex-M33) image this app embeds in its own ELF, when it
    /// has one. Null for a single-core app, which is every app whose whole
    /// CMakeLists is one ra8_add_app() call. See cpu1_image.zig.
    cpu1: ?cpu1_image.Cpu1Image = null,
    /// The per-function stack-frame budget this app names in `STACK_BYTES`,
    /// which ra8_add_app() forwards to ra8_target_enable_project_warnings() as
    /// `STACK_USAGE_BYTES` and which becomes `-Wstack-usage=<n>` on every one
    /// of the app's own translation units. The default is 2200, not the 2048
    /// cmake/ra8_warnings.cmake falls back to: ra8_add_app() always passes the
    /// keyword, so its own default is the one an app gets by saying nothing.
    stack_bytes: u32 = 2200,
    /// Shared helper translation units the app names in `EXTRA_SRCS`, in the
    /// order it names them, spelled repo-relative. Each one is compiled INTO
    /// this app (so it meets the full project warning profile, unlike a
    /// library archive) and each one's PARENT DIRECTORY goes on the include
    /// path, so a header sitting beside the helper resolves. The directory
    /// half is the silent one: the sources alone link fine right up until a
    /// helper includes its own co-located header.
    extra_srcs: []const []const u8 = &.{},
    /// False for an app that does not LINK in a Debug configure, measured on
    /// BOTH build systems rather than assumed. secure_boot_hil is the first:
    /// its 206 first-party TUs plus the referenced members of its 77-TU crypto
    /// archive overflow the 128 KiB MRAM region by ~29.9 KB at -O0 -g3, and
    /// CMake's own standalone configure of the same app fails the same way
    /// with the same message (see the issue linked from the app's entry). The
    /// graph still COMPILES every one of its translation units, which is what
    /// the source and flag rules are about; only the final link is held back,
    /// so `zig build arm` reports a known upstream-equal limit instead of
    /// going red on a defect it did not introduce.
    links_in_debug: bool = true,
    /// What the app's own CMakeLists adds on top of its ra8_add_app() call:
    /// extra defines, extra include directories, and a vendored static
    /// library it declares and links. Null for an app that is one
    /// ra8_add_app() call and nothing else. See app_local.zig.
    local: app_local_mod.AppLocal = .{},
    /// Vendored middleware named in `USES`, in the order the app names it.
    /// Each one compiles its own translation units at its own bar AND exports
    /// include directories, defines, and link options onto this app. See
    /// middleware.zig.
    uses: []const []const u8 = &.{},
    /// The subset of `libs/ra8_nsc/src` this app compiles, named exactly as
    /// `NSC_SRCS` names it (bare file names, no directory). Empty means the
    /// app named none, and cmake/ra8_app/sources.cmake then globs the whole
    /// directory -- which is what all six pre-#1096 apps get.
    ///
    /// The narrowing exists because the NSC veneers are not one set: an app
    /// pulls the CGC veneers without dragging in ra8_nsc_comms/ra8_nsc_eth,
    /// which do not compile under its secure configuration. Nothing in the
    /// directory listing says so, and globbing anyway puts nine extra
    /// secure-world translation units into an image CMake never put them in.
    nsc_srcs: []const []const u8 = &.{},
    /// True when a standalone configure of this app has RA8_TRUSTZONE_ENABLE
    /// ON, which is an app-local `option(... ON)` in its own CMakeLists and
    /// not the repo-root default (OFF). It adds a define and -mcmse to every
    /// one of the app's translation units and to its link; see
    /// arm_flags.trust_zone for what each of those actually buys.
    trust_zone: bool = false,
    /// The CMSE import library the app's own CMakeLists asks the link to
    /// emit, named as the file name CMake gives it. Null for every app that
    /// is not the secure half of a two-project TrustZone build.
    cmse_implib: ?[]const u8 = null,
    /// The SECOND executable of a two-project TrustZone build: the Non-Secure
    /// image, declared by the app's own CMakeLists with a raw add_executable()
    /// and linked against the import library the secure link above emits. Null
    /// for every app that is not the secure half of such a build. See
    /// ns_image.zig.
    ns: ?ns_image_mod.NsImage = null,
};

/// A name in `LIBS` that contributes translation units from somewhere other
/// than `libs/<name>/src`. `ra8_io_bus` is the only one today and it is the
/// sharp case: there is no `libs/ra8_io_bus` directory at all, so the LIBS
/// loop's glob for it is silently empty and a graph built from the directory
/// listing compiles NOTHING for it. cmake/ra8_app/sources.cmake instead
/// compiles just the SPI/I2C bus facades out of `libs/ra8_io` and puts
/// `libs/ra8_io/inc` on the include path -- the bus contracts without the rest
/// of the ra8_io fabric (no ra8_fs, no ra8_sdmmc_spi, no stream layer).
///
/// Encoded here for the same reason the board opt-in gate is: nothing on disk
/// says a library with no directory contributes sources, and getting it wrong
/// is a link failure at the end of a 200-TU cross-build rather than anything
/// that names the rule.
pub const LibraryAlias = struct {
    name: []const u8,
    /// Skipped when the app also names one of these: the fuller library
    /// already compiles the same translation units.
    superseded_by: []const []const u8,
    source_dir: []const u8,
    source_prefixes: []const []const u8,
    include_dir: []const u8,
};

pub const library_aliases = [_]LibraryAlias{
    .{
        .name = "ra8_io_bus",
        .superseded_by = &.{"ra8_io"},
        .source_dir = "libs/ra8_io/src",
        .source_prefixes = &.{ "ra8_io_spi_bus", "ra8_io_i2c_bus" },
        .include_dir = "libs/ra8_io/inc",
    },
};

/// A translation unit a named library globs in that ra8_add_app() then drops
/// again unless the app ALSO names the library whose headers it reaches for.
/// `libs/ra8_io/src/ra8_io_blockdev_vsource.c` is the case today: a bare
/// `ra8_io` globs it in, but it includes ra8_vsource.h from ra8_mem, so
/// cmake/ra8_app/sources.cmake keeps it (and puts `libs/ra8_mem/inc` on the
/// include path) only for an app that declares `ra8_mem` as well.
///
/// Encoded for the same reason the board gate is: a directory-listing graph
/// compiles the unit for every `ra8_io` consumer, and it does not fail on the
/// missing header -- `libs/ra8_mem/inc` happens to be reachable through
/// nothing at all, so what you get is an extra object in an image CMake never
/// put it in. #1068 measured the drop arm on ra8_io_swap_demo (which names
/// `ra8_io` and not `ra8_mem`): 261 units against CMake's 260, this one
/// either-only. The KEEP arm is encoded from the listfile and is not yet
/// measured -- no app in the table names `ra8_mem`.
pub const LibrarySourceGate = struct {
    /// The gated unit, repo-relative.
    source: []const u8,
    /// The library the app must also name in `LIBS` to keep it.
    satisfied_by: []const u8,
    /// The include directory that same declaration adds.
    include_dir: []const u8,
};

pub const library_source_gates = [_]LibrarySourceGate{
    .{
        .source = "libs/ra8_io/src/ra8_io_blockdev_vsource.c",
        .satisfied_by = "ra8_mem",
        .include_dir = "libs/ra8_mem/inc",
    },
};

/// True when `source` is a library unit whose companion library the app does
/// not declare.
pub fn isGatedOutLibrarySource(app: CrossApp, source: []const u8) bool {
    for (library_source_gates) |gate| {
        if (!std.mem.eql(u8, gate.source, source)) continue;
        return !declaresLibrary(app, gate.satisfied_by);
    }
    return false;
}

/// Board translation units that are opt-in rather than universal, and the
/// library an app must name in `LIBS` to get them. The rule lives in
/// cmake/ra8_app/sources.cmake, NOT in the board directory: the board glob
/// compiles every BSP unit into every app, and these two are then filtered
/// back out because they reach outside the unconditional include set
/// (`..._console_stream.c` hands back an `ra8_io_stream_t` and needs the full
/// `ra8_io`; `..._touch.c` binds GT911 through the ra8_io I2C facade, so
/// either `ra8_io` or `ra8_io_bus` will do).
///
/// Encoded here because a directory listing cannot tell you about it: globbing
/// the board's src/ and stopping there compiles `..._console_stream.c` into an
/// app that never opted in, and it fails on a missing ra8_io_stream.h rather
/// than on anything that names the gate.
pub const BoardOptIn = struct {
    suffix: []const u8,
    satisfied_by: []const []const u8,
};

pub const board_opt_in_sources = [_]BoardOptIn{
    .{ .suffix = "_console_stream.c", .satisfied_by = &.{"ra8_io"} },
    .{ .suffix = "_touch.c", .satisfied_by = &.{ "ra8_io", "ra8_io_bus" } },
};

// ===========================================================================
// The app table (#936, widened by #1021, #1036, #1044, #1054, #1068)
// ===========================================================================

/// The two warnings the app's own CMakeLists suppresses on the VENDORED USBX
/// sources and nowhere else. Measured there one flag at a time over all 187
/// USBX TUs this image compiles: both fire, and the third the audit started
/// with (-Wno-redundant-decls) fired on none and was deleted. Named once here
/// because two vendored sets carry the same pair and the first-party bridge
/// beside them carries neither.
const usbx_suppressions = [_][]const u8{
    "-Wno-discarded-qualifiers",
    "-Wno-cast-align",
};

pub const cross_apps = [_]CrossApp{
    .{
        .name = "blink_hal",
        .dir = "examples/ek_ra8d2/hw_validated/hil/blink_hal",
        .board = "libs/ra8_board_ek_ra8d2",
        // ra8_add_app() falls back to the board's canonical single-core map
        // when the app has no linker_script.ld of its own, which this app
        // does not.
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        // blink_hal names no LIBS at all: it is the universal first-party set
        // and nothing else, which is what made it the right FIRST app to
        // cross-build here. The `zig_libraries` hook below is wired and
        // exercised by an empty list; an app that links a migrated Zig ARCHIVE
        // cannot be cross-built by either build system yet, see #948.
        .libraries = &.{},
        .zig_libraries = &.{},
    },
    .{
        // The second app, and the reason there is a table here at all: one app
        // cannot distinguish a rule that generalises from a constant that
        // happens to be right. iic_b_facade_demo names two libraries in LIBS
        // and so takes the OTHER arm of every source rule blink_hal takes --
        // the board opt-in gate keeps `..._touch.c` instead of dropping it, a
        // library with no directory of its own contributes six translation
        // units, and the include path grows a directory. It links no migrated
        // Zig archive, so #948 does not block it.
        .name = "iic_b_facade_demo",
        .dir = "examples/ek_ra8d2/hw_validated/hil/iic_b_facade_demo",
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{ "ra8_board_ek_ra8d2", "ra8_io_bus" },
        .zig_libraries = &.{},
    },
    .{
        // The third app, for the rule NEITHER of the first two can see: both
        // of them keep exactly one translation unit under their own `src/`
        // (main.c), so every app-local decision ra8_add_app() makes was
        // unobservable. cpu1_pingpong keeps three, and each takes a different
        // arm:
        //
        //   src/main.c            the primary entry point, added first.
        //   src/trustzone_init.c  an app-local override of a BOOT unit, so the
        //                         board's src/boot copy must NOT be linked --
        //                         the other arm of the per-app boot resolver,
        //                         which both earlier apps took the board side
        //                         of, five times each.
        //   src/cpu1_main.c       named in AUX_SRCS: the Cortex-M33 entry
        //                         point for the SECOND image this app builds,
        //                         which must be kept out of the M85 image
        //                         entirely.
        //
        // It also ships an `inc/` of its own (the dual-core mailbox contract
        // shared_pingpong.h), which is the first directory on CMake's include
        // path and had never been exercised either.
        //
        // No LIBS, no USES, no migrated Zig archive, so #948 does not block it.
        //
        // The app's CMakeLists hand-rolls a SECOND executable for the M33
        // (cpu1_pingpong_cpu1.elf, four TUs at -mcpu=cortex-m33, its own
        // linker script) and objcopies it into the M85 image as a .cpu1_image
        // blob. That is app-local CMake outside ra8_add_app(), and #1044 is
        // the slice that brought it into the graph: see the .cpu1 field below.
        .name = "cpu1_pingpong",
        .dir = "examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong",
        .board = "libs/ra8_board_ek_ra8d2",
        // The app ships its own linker_script.ld (it pins .cpu1_image at
        // ORIGIN(MRAM_CPU1)), so ra8_add_app() takes that one over the board's.
        .linker_script = "examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong/linker_script.ld",
        .libraries = &.{},
        .zig_libraries = &.{},
        .aux_srcs = &.{"src/cpu1_main.c"},
        // The M33 half of this app (#1044). Its entry TU is the same file
        // AUX_SRCS keeps out of the M85 set above: one file, two images.
        .cpu1 = .{
            .entry_source = "src/cpu1_main.c",
            .shared_sources = &.{
                "libs/ra8_hal/src/ra8_ipc.c",
                "libs/ra8_core/src/ra8_log.c",
                "libs/ra8_core/src/ra8_scb.c",
            },
            .linker_script = "linker_script_cpu1.ld",
        },
    },
    .{
        // The fourth app, for the whole dimension the first three cannot see:
        // vendored MIDDLEWARE. None of them names `USES`, so the graph had
        // never compiled a line of it, and all four things ra8_add_app() does
        // with a middleware dependency were unobserved -- its own source set
        // and flag bar, the include directories and defines it exports onto
        // the app's TUs, the options it forces onto the link, and the fact
        // that the app links an ARCHIVE rather than a bag of objects.
        //
        // threadx_blink is the smallest app that names one: `USES threadx`
        // and nothing else, no LIBS, no EXTRA_SRCS, no migrated Zig archive,
        // so #948 does not block it and its first-party set is byte-for-byte
        // blink_hal's 200 TUs. Everything that differs between the two apps
        // is the middleware, which is what makes it the right fourth app.
        .name = "threadx_blink",
        .dir = "examples/ek_ra8d2/hw_validated/hil/threadx_blink",
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "examples/ek_ra8d2/hw_validated/hil/threadx_blink/linker_script.ld",
        .libraries = &.{},
        .zig_libraries = &.{},
        .uses = &.{"threadx"},
    },
    .{
        // The fifth app, for a rule the graph has been treating as a CONSTANT.
        // All four apps above take the default `STACK_BYTES` (2200), so the
        // first-party warning profile hard-coded `-Wstack-usage=2200` as if
        // every app shared one frame budget. 130 apps do; roughly ninety do
        // not (27 at 4096, 19 at 4000, 13 at 16384, 12 at 32768, 11 at 8192,
        // and a tail besides). Both directions of getting it wrong are quiet:
        // a bigger budget means the graph holds the app to a TIGHTER bar than
        // CMake and a legitimate frame fails only here, and a smaller one
        // means the graph never rejects a frame CMake does.
        //
        // ra8_io_swap_demo names `STACK_BYTES 4096` and is otherwise the
        // plainest app that can carry the rule: no USES, no EXTRA_SRCS, no
        // AUX_SRCS, no app-local CMake, no migrated Zig archive, so #948 does
        // not block it.
        //
        // It also opens the one arm of the board opt-in gate (#936) that no
        // app in this table has opened. `..._console_stream.c` is gated on the
        // app naming `ra8_io`, and until now every app here has been on the
        // shut side of that gate, so only its DROP was ever observed. This app
        // names `ra8_io` and keeps the unit. Its four libraries are also the
        // first set where one of them (`ra8_usb_pal`) is already in the
        // universal include set, so the include path has to deduplicate
        // rather than repeat the directory.
        .name = "ra8_io_swap_demo",
        .dir = "examples/ek_ra8d2/hw_pending/ra8_io_swap_demo",
        .board = "libs/ra8_board_ek_ra8d2",
        // No linker_script.ld of its own, so the board's canonical single-core
        // map, the same fallback blink_hal takes.
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{ "ra8_io", "ra8_fs", "ra8_sdmmc_spi", "ra8_usb_pal" },
        .zig_libraries = &.{},
        .stack_bytes = 4096,
    },
    .{
        // The sixth app, and the first whose own CMakeLists does real work of
        // its own beyond one ra8_add_app() call plus a define. secure_boot_hil
        // is the app for two rules nothing before it could see:
        //
        //   EXTRA_SRCS. Five shared helper TUs pulled in from two libraries
        //   the app does NOT name in LIBS, each compiled into the app at the
        //   full project profile, each contributing its own parent directory
        //   to the include path. 8 app CMakeLists name EXTRA_SRCS; none of the
        //   five apps before this one did, so the keyword was dead code in the
        //   graph and the helper would have been missing from the image.
        //
        //   A vendored static library the app declares itself (tfpsa_sb: 77
        //   tf-psa-crypto TUs) whose PUBLIC defines and SYSTEM include
        //   directories land on the app's own translation units. See
        //   app_local.zig for why three of those four effects are silent.
        //
        // No USES, so the first-party set is the same shape as blink_hal's and
        // everything differing IS the two rules above. It names no migrated
        // Zig archive, so #948 does not block it. STACK_BYTES 32768 is the
        // second non-default frame budget in the table (#1068).
        .name = "secure_boot_hil",
        .dir = "examples/ek_ra8d2/hw_validated/hil/secure_boot_hil",
        .board = "libs/ra8_board_ek_ra8d2",
        // This app ships its own linker_script.ld, so no board fallback.
        .linker_script = "examples/ek_ra8d2/hw_validated/hil/secure_boot_hil/linker_script.ld",
        .libraries = &.{"ra8_board_ek_ra8d2"},
        .zig_libraries = &.{},
        .stack_bytes = 32768,
        // Neither build system links this app in a Debug configure: measured
        // 122.79% of MRAM under CMake (overflow 29868 bytes) and the same
        // failure from the graph (overflow 29860 bytes), same linker, same
        // message. Filed separately; every TU still compiles here.
        .links_in_debug = false,
        .extra_srcs = &.{
            "libs/ra8_psa_crypto/src/ra8_psa_crypto.c",
            "libs/ra8_dfu/src/ra8_rot.c",
            "libs/ra8_dfu/src/ra8_dfu_antirollback.c",
            "libs/ra8_dfu/src/ra8_dfu_boot.c",
            "libs/ra8_dfu/src/ra8_dfu_launch.c",
        },
        .local = .{
            .defines = &.{"-DRA8_ENABLE_ROOT_OF_TRUST"},
            .include_dirs = &.{
                "libs/ra8_psa_crypto/inc",
                "libs/ra8_dfu/inc",
            },
            .vendored = .{
                .name = "tfpsa_sb",
                .source_dirs = &.{
                    "libs/third_party/tf-psa-crypto/core",
                    "libs/third_party/tf-psa-crypto/drivers/builtin/src",
                    "libs/third_party/tf-psa-crypto/platform",
                    "libs/third_party/tf-psa-crypto/utilities",
                    "libs/third_party/tf-psa-crypto/extras",
                },
                .system_include_dirs = &.{
                    "libs/third_party/tf-psa-crypto/include",
                    "libs/third_party/tf-psa-crypto/drivers/builtin/include",
                    "libs/third_party/tf-psa-crypto/core",
                    "libs/third_party/tf-psa-crypto/dispatch",
                    "libs/third_party/tf-psa-crypto/drivers/builtin/src",
                    "libs/third_party/tf-psa-crypto/platform",
                    "libs/third_party/tf-psa-crypto/utilities",
                    "libs/third_party/tf-psa-crypto/extras",
                    "libs/third_party/mbedtls/include",
                    "port/mbedtls/inc",
                },
                .defines = &.{
                    "-DMBEDTLS_CONFIG_FILE=\"mbedtls_config.h\"",
                    "-DTF_PSA_CRYPTO_CONFIG_FILE=\"tf_psa_crypto_config.h\"",
                    "-DMBEDTLS_PLATFORM_MEMORY",
                    "-DMBEDTLS_MEMORY_BUFFER_ALLOC_C",
                },
                // Not a diagnostic switch: it changes code generation, which
                // is why it is the one option left after the audit recorded in
                // the app's CMakeLists deleted the two -Wno- flags beside it.
                .compile_options = &.{"-fno-strict-aliasing"},
            },
        },
    },
    .{
        // The SEVENTH app, and the first TrustZone one. Everything above was
        // built with RA8_TRUSTZONE_ENABLE OFF, so three rules had never been
        // taken: NSC_SRCS narrowing the NSC set, the define and -mcmse the
        // option adds to every app translation unit and to the link, and the
        // CMSE import library the app's own CMakeLists asks for. It is the
        // only app in the tree naming NSC_SRCS. Its first-party set is
        // otherwise blink_hal's shape, so everything differing between the
        // two images IS those rules: 200 - 9 NSC + 1 ra8_tz_secure_boot = 192.
        //
        // It also overrides TWO boot units (src/system_init.c and
        // src/trustzone_init.c), where cpu1_pingpong overrode one, and its
        // AUX_SRCS name the three ns_*.c files that belong to the SEPARATE
        // Non-Secure executable, which this slice deliberately leaves out.
        // It links no migrated Zig archive, so #948 does not block it.
        .name = "tz_nsc_cgc_usb",
        .dir = "examples/ek_ra8d2/hil_needs_revalidation/tz_nsc_cgc_usb",
        .board = "libs/ra8_board_ek_ra8d2",
        // This app ships its own script: the secure image is 512K of MRAM
        // with the NS image's load home carved out of the rest, which the
        // board's canonical single-core map has no notion of.
        .linker_script = "examples/ek_ra8d2/hil_needs_revalidation/tz_nsc_cgc_usb/linker_script.ld",
        .libraries = &.{"ra8_tz_secure_boot"},
        .zig_libraries = &.{},
        .aux_srcs = &.{ "src/ns_main.c", "src/ns_usb.c", "src/ns_usb_host.c" },
        .nsc_srcs = &.{"ra8_nsc_cgc.c"},
        .trust_zone = true,
        .cmse_implib = "tz_nsc_cgc_usb_cmse_import.o",
        // The Non-Secure half (#1111). The three ns_*.c files AUX_SRCS keeps
        // out of the secure image above are this image's own sources, which is
        // the same one-file-two-images shape cpu1_pingpong has (#1044) with a
        // whole vendored USB stack and an RTOS variant on top.
        .ns = .{
            .name = "tz_nsc_cgc_usb_ns",
            .app_sources = &.{ "src/ns_main.c", "src/ns_usb.c", "src/ns_usb_host.c" },
            .vendored = &.{
                .{
                    .dir = "libs/third_party/usbx/common/core/src",
                    .excluded_prefixes = &.{ "ux_dcd_sim_slave_", "ux_hcd_sim_host_" },
                    .suppressions = &usbx_suppressions,
                },
                .{
                    .dir = "libs/third_party/usbx/common/usbx_device_classes/src",
                    .prefix = "ux_device_class_cdc_acm_",
                    .suppressions = &usbx_suppressions,
                },
                // The first-party USBX<->ra8_usb bridge, globbed as the app
                // globs it (ux_dcd_ra8_usb*.c, so neither ux_hcd_ra8_usb.c nor
                // the storage class TU beside them joins) and WITHOUT the
                // suppressions above: it is ours and keeps the full bar.
                .{ .dir = "port/usbx/src", .prefix = "ux_dcd_ra8_usb" },
            },
            // Named one by one, in the app's order. ra8_time.c is deliberately
            // absent: its ra8_time_init reprograms the SysTick ThreadX owns,
            // and ns_usb.c supplies the ThreadX-backed ra8_delay_ms instead.
            .private_sources = &.{
                "libs/ra8_hal/src/ra8_usb.c",
                "libs/ra8_hal/src/ra8_usb_phy.c",
                "libs/ra8_hal/src/ra8_usb_device.c",
                "libs/ra8_hal/src/ra8_usb_xfer.c",
                "libs/ra8_hal/src/ra8_usb_irq.c",
                "libs/ra8_hal/src/ra8_usb_host_ctrl.c",
                "libs/ra8_hal/src/ra8_usb_host_bulk.c",
                "libs/ra8_hal/src/ra8_mstp.c",
                "libs/ra8_core/src/ra8_log.c",
                "libs/ra8_core/src/ra8_scb.c",
            },
            // RA8_PERIPH_NS_ALIAS routes ra8_usb/ra8_mstp at the IDAU
            // bit[28]=1 Non-secure alias; RA8_USB_POLLED_ONLY keeps the DCD
            // off the Secure-attributed USB NVIC line.
            .defines = &.{
                "-DRA8_TRUSTZONE_ENABLE",
                "-DRA8_PERIPH_NS_ALIAS",
                "-DRA8_USB_POLLED_ONLY",
            },
            .app_include_dirs = &.{ "inc", "src" },
            .include_dirs = &.{
                "libs/ra8_core/inc",
                "libs/ra8_hal/inc",
                "libs/ra8_nsc/inc",
                "port/usbx/inc",
                "libs/ra8_usb_pal/inc",
                "libs/ra8_board_ek_ra8d2/inc",
            },
            .system_include_dirs = &.{
                "libs/third_party/usbx/common/core/inc",
                "libs/third_party/usbx/common/usbx_device_classes/inc",
                "libs/third_party/usbx/ports/cortex_m33/gnu/inc",
            },
            .uses = "threadx_ns",
            .stack_bytes = 2200,
            .linker_script = "ns_image.ld",
            .link_flags = &.{"-nostartfiles"},
        },
    },
    .{
        // The eighth app, and the first that is not an ek_ra8d2 app at all.
        // `ra8_add_app(BOARD ra8p1)` is a keyword none of the seven above
        // names, so everything keyed on the board was indistinguishable from a
        // constant: the BSP source directory, the board include directory, the
        // board's src/boot copies the per-app resolver falls back to, and the
        // board linker script all read `libs/ra8_board_ek_ra8d2` off a literal.
        // Two apps in the tree select the RA8P1 layer; this is the smaller.
        //
        // It also selects a different CMAKE TOOLCHAIN FILE, which is the half
        // of the choice that is not visible in the app's CMakeLists at all:
        // cmake/toolchain-ra8p1.cmake includes the RA8D2 body verbatim and
        // appends the DP-FPU override and the device define. See device.zig
        // for why both are silent when missed.
        //
        // No LIBS, no USES, no EXTRA_SRCS, no migrated Zig archive (so #948
        // does not block it), and it ships its own linker_script.ld and its
        // own src/vector_table.c -- the second app in the table to override a
        // boot unit, and the first to override one on a board layer whose
        // src/boot does not carry that file at all.
        .name = "blink_ra8p1",
        .dir = "examples/ra8p1_foundation/blink_ra8p1",
        .board = "libs/ra8_board_ra8p1",
        .linker_script = "examples/ra8p1_foundation/blink_ra8p1/linker_script.ld",
        .libraries = &.{},
        .zig_libraries = &.{},
    },
};

/// The universal first-party source set ra8_add_app() globs into every app,
/// plus the board layer this app selects. Each entry is globbed for `*.c`
/// non-recursively, exactly as the CMake `file(GLOB ...)` calls do.
const cross_source_dirs = [_][]const u8{
    "libs/ra8_core/src",
    "libs/ra8_hal/src",
    "libs/ra8_nsc/src",
    "libs/ra8_net_pal/src",
    "libs/ra8_usb_pal/src",
    "libs/ra8_secure_app/src",
    // The board layer's own src/ is NOT listed here: it is `<app.board>/src`,
    // appended per app by crossSources() below. Every app cross-built before
    // #1131 is an ek_ra8d2 app, so the directory sat in this list looking like
    // a constant; `ra8_add_app(BOARD ra8p1)` resolves it to a different layer
    // entirely, and a hard-coded entry would compile the RA8D2 BSP into an
    // RA8P1 image and omit the RA8P1 one.
};

/// The one directory in cross_source_dirs an app can narrow. Named so the
/// glob loop and the NSC_SRCS rule cannot drift apart about which directory
/// the subset applies to.
pub const nsc_source_dir = "libs/ra8_nsc/src";

/// Whether `relpath` is an NSC translation unit this app does NOT compile.
/// Pure, so both arms assert directly in test-zig instead of only through a
/// 192-TU cross-build: an app that names no NSC_SRCS compiles all ten, and an
/// app that names a subset compiles exactly that subset.
pub fn isGatedOutNscSource(app: CrossApp, relpath: []const u8) bool {
    if (app.nsc_srcs.len == 0) return false;
    if (!std.mem.startsWith(u8, relpath, nsc_source_dir ++ "/")) return false;
    const name = std.fs.path.basename(relpath);
    for (app.nsc_srcs) |named| {
        if (std.mem.eql(u8, named, name)) return false;
    }
    return true;
}

/// Boot translation units resolved per app: the app's own copy under `src/`
/// when it has one, otherwise the board layer's copy under `src/boot/`. This
/// is the per-app override rule in ra8_add_app(), and it is why
/// `libs/ra8_board_ek_ra8d2/src/boot` is NOT in cross_source_dirs above -- a
/// blind glob of that directory would link the board's vector table into an
/// app that ships its own.
const cross_boot_sources = [_][]const u8{
    "vector_table.c",
    "system_init.c",
    "secure_exception.c",
    "nmi_exception.c",
    "trustzone_init.c",
};

/// True when `basename` is one of the boot units resolved per app above. The
/// app-local glob below has to skip them, because the resolver already picked
/// the app copy or the board copy and adding the app copy a second time is a
/// duplicate object in the link.
fn isBootSource(basename: []const u8) bool {
    for (cross_boot_sources) |boot| {
        if (std.mem.eql(u8, boot, basename)) return true;
    }
    return false;
}

/// True when `relative_path` (relative to the app directory) is named in this
/// app's `AUX_SRCS`.
///
/// AUX_SRCS is the rule with the least presence on disk of any in this file:
/// the file sits under the app's own `src/`, looks exactly like every other
/// app-local helper, and compiles cleanly -- into the WRONG IMAGE. A dual-core
/// app keeps its Cortex-M33 entry point beside its M85 one (`src/cpu1_main.c`),
/// names it in AUX_SRCS, and ra8_add_app() then drops it from the primary
/// image's source list; a second executable elsewhere in the app's CMakeLists
/// compiles it for the M33. A graph that globs `<app>/src/*.c` and stops there
/// links two `main`-shaped entry points and two copies of the shared state into
/// the M85 image.
pub fn isAuxSource(app: CrossApp, relative_path: []const u8) bool {
    for (app.aux_srcs) |aux| {
        if (std.mem.eql(u8, aux, relative_path)) return true;
    }
    return false;
}

/// Whether a source the app-local glob turned up is compiled into this image
/// BY THAT GLOB. Three are not, each for its own reason, and the reasons are
/// why this is a function with tests rather than an inline condition:
///
///   main.c          already added, as the first object in the link.
///   a boot unit     already placed by the per-app boot resolver, which chose
///                   between this copy and the board's; taking it again is a
///                   duplicate object.
///   an AUX_SRCS     belongs to another image entirely (see isAuxSource).
///
/// `relative_path` is spelled relative to the app directory ("src/main.c"),
/// the way AUX_SRCS spells it in the app's CMakeLists.
pub fn appLocalIsCompiled(app: CrossApp, relative_path: []const u8) bool {
    const basename = std.fs.path.basename(relative_path);
    if (std.mem.eql(u8, basename, "main.c")) return false;
    if (isBootSource(basename)) return false;
    if (isAuxSource(app, relative_path)) return false;
    return true;
}

/// Which copy of one boot unit this app links: its own when it ships one,
/// otherwise the board layer's. Pure so both arms can be asserted directly;
/// the caller does the one filesystem probe and passes the answer in.
pub fn bootSourcePath(
    allocator: std.mem.Allocator,
    app: CrossApp,
    boot: []const u8,
    app_has_copy: bool,
) []const u8 {
    return if (app_has_copy)
        std.fmt.allocPrint(allocator, "{s}/src/{s}", .{ app.dir, boot }) catch @panic("OOM")
    else
        std.fmt.allocPrint(allocator, "{s}/src/boot/{s}", .{ app.board, boot }) catch @panic("OOM");
}

/// Include path, in the order ra8_add_app() adds it. Order is preserved
/// because a header shadowed by an earlier directory resolves differently, and
/// a parity claim that only holds for one ordering is not a parity claim.
const cross_include_dirs = [_][]const u8{
    "libs/ra8_core/inc",
    "libs/ra8_hal/inc",
    "libs/ra8_net_pal/inc",
    "libs/ra8_usb_pal/inc",
    "libs/ra8_nsc/inc",
    "libs/ra8_secure_app/inc",
    // `<app.board>/inc` closes this list, appended per app by
    // crossIncludeDirs() below rather than spelled here: see the note on
    // cross_source_dirs. It is LAST of the universal set and ahead of every
    // library directory, so a board header shadows a library's same-named one.
};

/// Collect `*.c` from one directory, sorted, so the link order is stable
/// across machines and two builds of the same tree produce the same ELF.
pub fn collectCSources(b: *std.Build, dir_path: []const u8, out: *std.ArrayList([]const u8)) void {
    var dir = b.build_root.handle.openDir(dir_path, .{ .iterate = true }) catch |err| {
        std.debug.panic("ra8: cannot read source directory '{s}': {s}", .{ dir_path, @errorName(err) });
    };
    defer dir.close();

    var names = std.ArrayList([]const u8).init(b.allocator);
    var it = dir.iterate();
    while (it.next() catch |err| {
        std.debug.panic("ra8: cannot walk '{s}': {s}", .{ dir_path, @errorName(err) });
    }) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".c")) continue;
        names.append(b.dupe(entry.name)) catch @panic("OOM");
    }
    std.mem.sort([]const u8, names.items, {}, struct {
        fn lessThan(_: void, a: []const u8, c: []const u8) bool {
            return std.mem.lessThan(u8, a, c);
        }
    }.lessThan);
    for (names.items) |name| {
        out.append(b.fmt("{s}/{s}", .{ dir_path, name })) catch @panic("OOM");
    }
}

/// True when `app` names `library` in its `LIBS`.
pub fn declaresLibrary(app: CrossApp, library: []const u8) bool {
    for (app.libraries) |declared| {
        if (std.mem.eql(u8, declared, library)) return true;
    }
    return false;
}

/// Every C translation unit in the app's link, in ra8_add_app()'s own order:
/// the app's main.c, the resolved boot files, the globbed universal set, then
/// whatever the app's `LIBS` add on top.
pub fn crossSources(b: *std.Build, app: CrossApp) []const []const u8 {
    var sources = std.ArrayList([]const u8).init(b.allocator);

    sources.append(b.fmt("{s}/src/main.c", .{app.dir})) catch @panic("OOM");

    for (cross_boot_sources) |boot| {
        const app_copy = b.fmt("{s}/src/{s}", .{ app.dir, boot });
        const app_has_copy = if (b.build_root.handle.access(app_copy, .{})) |_| true else |_| false;
        sources.append(bootSourcePath(b.allocator, app, boot, app_has_copy)) catch @panic("OOM");
    }

    // Everything else the app keeps under `src/`: ra8_add_app() globs that
    // directory and compiles what is left after main.c (added above), the boot
    // units the resolver just placed, and the app's AUX_SRCS are taken out of
    // it. Globbing without those three subtractions is not a smaller parity
    // claim, it is a different image.
    var app_local = std.ArrayList([]const u8).init(b.allocator);
    collectCSources(b, b.fmt("{s}/src", .{app.dir}), &app_local);
    for (app_local.items) |source| {
        if (!appLocalIsCompiled(app, source[app.dir.len + 1 ..])) continue;
        sources.append(source) catch @panic("OOM");
    }

    // EXTRA_SRCS, appended in the order the app names them and BEFORE the
    // library globs, which is the order cmake/ra8_app/sources.cmake builds the
    // list in and therefore the order the objects reach the linker.
    for (app.extra_srcs) |source| sources.append(source) catch @panic("OOM");

    for (cross_source_dirs) |dir_path| {
        // NSC_SRCS replaces the glob of libs/ra8_nsc/src with exactly the
        // files the app named, in the order it named them. See
        // CrossApp.nsc_srcs: this is a subtraction no listing shows, and the
        // build succeeds either way.
        if (std.mem.eql(u8, dir_path, nsc_source_dir) and app.nsc_srcs.len > 0) {
            for (app.nsc_srcs) |name| {
                sources.append(b.fmt("{s}/{s}", .{ nsc_source_dir, name })) catch @panic("OOM");
            }
            continue;
        }
        collectCSources(b, dir_path, &sources);
    }
    // The board layer this app selected, last of the universal set and
    // resolved per app rather than hard-coded (#1131).
    collectCSources(b, b.fmt("{s}/src", .{app.board}), &sources);

    // Named libraries. A library with a directory of its own contributes
    // `libs/<name>/src/*.c`; a board named in LIBS contributes the same set the
    // universal glob already took, minus src/boot (the per-app override above
    // decides those), so it lands as duplicates and is deduplicated below.
    for (app.libraries) |library| {
        const library_dir = b.fmt("libs/{s}/src", .{library});
        const exists = if (b.build_root.handle.access(library_dir, .{})) |_| true else |_| false;
        if (exists) collectCSources(b, library_dir, &sources);
    }
    for (library_aliases) |alias| {
        if (!declaresLibrary(app, alias.name)) continue;
        var superseded = false;
        for (alias.superseded_by) |fuller| {
            if (declaresLibrary(app, fuller)) superseded = true;
        }
        if (superseded) continue;
        var aliased = std.ArrayList([]const u8).init(b.allocator);
        collectCSources(b, alias.source_dir, &aliased);
        for (aliased.items) |source| {
            const name = std.fs.path.basename(source);
            for (alias.source_prefixes) |prefix| {
                if (std.mem.startsWith(u8, name, prefix)) {
                    sources.append(source) catch @panic("OOM");
                    break;
                }
            }
        }
    }

    // Drop the opt-in board units this app did not opt into (see
    // board_opt_in_sources), then the duplicates a named board produces.
    var kept = std.ArrayList([]const u8).init(b.allocator);
    var seen = std.StringHashMap(void).init(b.allocator);
    for (sources.items) |source| {
        if (isGatedOutBoardSource(app, source)) continue;
        if (isGatedOutLibrarySource(app, source)) continue;
        if (seen.contains(source)) continue;
        seen.put(source, {}) catch @panic("OOM");
        kept.append(source) catch @panic("OOM");
    }
    return kept.items;
}

/// The app's include path, in the order ra8_add_app() adds it: the app's own
/// directories, the universal first-party set, then one directory per named
/// library that has headers.
pub fn crossIncludeDirs(b: *std.Build, app: CrossApp) []const []const u8 {
    var dirs = std.ArrayList([]const u8).init(b.allocator);
    // CMake adds `<app>/inc` UNCONDITIONALLY, and it is FIRST, ahead of the
    // app's own src/ and every library: an app-local header shadows a
    // same-named one further down the path. cpu1_pingpong is the app that
    // ships one (inc/shared_pingpong.h, the dual-core mailbox contract).
    //
    // It is emitted for an app that ships no inc/ too, which is a real
    // difference and not a formality: this list is also what
    // `zig build compile-db` writes, and a database row whose include path is
    // one directory short of the compiler's is a row clang-tidy resolves
    // differently than the build did. #1068 measured it on ra8_io_swap_demo,
    // which ships no inc/; the step spells an absent directory as a plain -I
    // string, because only an existing directory can be declared as a step
    // input.
    dirs.append(b.fmt("{s}/inc", .{app.dir})) catch @panic("OOM");
    dirs.append(b.fmt("{s}/src", .{app.dir})) catch @panic("OOM");
    dirs.appendSlice(&cross_include_dirs) catch @panic("OOM");
    dirs.append(b.fmt("{s}/inc", .{app.board})) catch @panic("OOM");

    for (app.libraries) |library| {
        const library_inc = b.fmt("libs/{s}/inc", .{library});
        const exists = if (b.build_root.handle.access(library_inc, .{})) |_| true else |_| false;
        if (exists) dirs.append(library_inc) catch @panic("OOM");
    }
    // The include directory a gated library unit's companion brings with it,
    // added where cmake/ra8_app/sources.cmake adds it: after the per-library
    // directories, before the alias one.
    for (library_source_gates) |gate| {
        if (declaresLibrary(app, gate.satisfied_by)) {
            dirs.append(gate.include_dir) catch @panic("OOM");
        }
    }
    for (library_aliases) |alias| {
        if (!declaresLibrary(app, alias.name)) continue;
        var superseded = false;
        for (alias.superseded_by) |fuller| {
            if (declaresLibrary(app, fuller)) superseded = true;
        }
        if (!superseded) dirs.append(alias.include_dir) catch @panic("OOM");
    }

    // One directory per EXTRA_SRCS entry, deduplicated, added LAST of
    // everything ra8_add_app() puts on the path (cmake/ra8_add_app.cmake
    // spells `${_ra8_extra_inc}` after `${_ra8_lib_inc}` in the same
    // target_include_directories call).
    for (app.extra_srcs) |source| {
        dirs.append(std.fs.path.dirname(source) orelse ".") catch @panic("OOM");
    }
    // Then whatever the app's own CMakeLists adds, which lands after
    // ra8_add_app() has already run.
    dirs.appendSlice(app.local.include_dirs) catch @panic("OOM");

    var kept = std.ArrayList([]const u8).init(b.allocator);
    var seen = std.StringHashMap(void).init(b.allocator);
    for (dirs.items) |dir_path| {
        if (seen.contains(dir_path)) continue;
        seen.put(dir_path, {}) catch @panic("OOM");
        kept.append(dir_path) catch @panic("OOM");
    }
    return kept.items;
}

/// True when `source` is a board unit whose companion library is absent from
/// the app's declared `LIBS`.
pub fn isGatedOutBoardSource(app: CrossApp, source: []const u8) bool {
    if (!std.mem.startsWith(u8, source, app.board)) return false;
    for (board_opt_in_sources) |gate| {
        if (!std.mem.endsWith(u8, source, gate.suffix)) continue;
        for (gate.satisfied_by) |required| {
            if (declaresLibrary(app, required)) return false;
        }
        return true;
    }
    return false;
}
