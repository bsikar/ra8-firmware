//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The cross-build app table: every example app the root Zig build graph
//! cross-compiles, and what each one is in the table FOR.
//!
//! Extracted from cross_sources.zig because that file reached the 1000-line
//! ceiling scripts/checks/check_file_size.py holds every Zig source to, and
//! the table is the coherent piece to lift out: cross_sources.zig keeps the
//! source-set RULES, this file keeps the apps that exercise them. Both halves
//! stay beside each other for the same reason they were together, so a new
//! entry is written against the rules it takes an arm of.
//!
//! One app cannot tell a rule that generalises from a constant that happens
//! to be right, which is the whole argument for a table rather than one
//! app: each entry below names the arms of `ra8_add_app()` that no earlier
//! entry takes, and the comment on it says which those are.

const std = @import("std");
const cpu1_image = @import("cpu1_image.zig");
const app_local_mod = @import("app_local.zig");
const ns_image_mod = @import("ns_image.zig");

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
    /// True when the app names `NO_NSC`, which is the THIRD arm of one
    /// decision cmake/ra8_app/sources.cmake makes about `libs/ra8_nsc/src`:
    /// glob the whole directory (every app that says nothing), compile the
    /// named subset (`NSC_SRCS`, above), or compile NONE of it. Only one app
    /// in the tree takes this arm.
    ///
    /// The include half does not follow the source half. `libs/ra8_nsc/inc`
    /// lives in the universal include set, so it stays on the path of every
    /// translation unit in an app that compiles none of those sources -- the
    /// app's headers still name the veneer prototypes it calls into the
    /// Non-Secure world with.
    ///
    /// Getting it wrong is silent. The app's own CMakeLists says the veneers
    /// are excluded because gcc rejects their struct-by-pointer arguments
    /// under -mcmse; compiling all ten at this app's exact flags and include
    /// path says otherwise today, with and without -mcmse (both clean). So a
    /// graph that ignored the keyword would not fail: it would put ten extra
    /// secure-world units into an image CMake never put them in, and link.
    no_nsc: bool = false,
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
    /// Libraries the app names in `OFF_TARGET_LIBS`, in the order it names
    /// them. Their translation units are compiled INTO this app like any
    /// `LIBS` unit, but with `RA8_OFF_TARGET` additionally defined on those
    /// units alone (cmake/ra8_add_app.cmake does it with
    /// set_source_files_properties, so it is a SOURCE-scope define, not a
    /// target one). One executable, two preprocessor views. See
    /// off_target_define for why forgetting it fails nothing at all.
    off_target_libs: []const []const u8 = &.{},
};

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
    .{
        // The ninth app, and the one that takes the last source-set keyword
        // ra8_add_app() has that nothing in this table names: OFF_TARGET_LIBS.
        //
        // It is not LIBS under another name. cmake/ra8_app/sources.cmake
        // collects those libraries into a list of their own and
        // cmake/ra8_add_app.cmake then hangs COMPILE_DEFINITIONS
        // "RA8_OFF_TARGET" on exactly those source files, so ONE executable
        // compiles its translation units at TWO preprocessor views: 200 at the
        // ordinary bar, and this library's 2 with the define on top. Every
        // source rule the graph modelled before this one is uniform across the
        // app.
        //
        // On THIS app both halves of the rule fail closed rather than
        // silently, which was measured and not assumed: without the define
        // both of the library's units stop compiling (its on-target arm
        // includes psa/crypto.h, which this app's include path does not
        // carry), and without `libs/ra8_psa_crypto/inc` the app's own main.c
        // stops compiling. See off_target_define for the configuration where
        // the same mistake IS silent. The include directory is the half worth
        // spelling out either way: it goes on the path of EVERY unit in the
        // app, not just the library's own, because the off-target loop feeds
        // the same `_ra8_lib_inc` list the LIBS loop does.
        //
        // No USES, no EXTRA_SRCS, no app-local CMake, no migrated Zig archive
        // (so #948 does not block it) and the default 2200-byte frame budget:
        // the first-party set is blink_hal's 200 units, so everything that
        // differs between the two apps IS this keyword (#1133).
        .name = "crypto_aes_demo",
        .dir = "examples/ek_ra8d2/hw_validated/hil/crypto_aes_demo",
        .board = "libs/ra8_board_ek_ra8d2",
        // No linker_script.ld of its own, so the board's canonical single-core
        // map, the same fallback blink_hal takes.
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{"ra8_board_ek_ra8d2"},
        .zig_libraries = &.{},
        .off_target_libs = &.{"ra8_psa_crypto"},
    },
    .{
        // The TENTH app, and the one that takes the last arm of the last
        // source-set keyword: `NO_NSC`. cmake/ra8_app/sources.cmake decides
        // the NSC set three ways and this table had two of them -- the whole
        // ten-unit glob every pre-#1096 app gets, and the named subset
        // tz_nsc_cgc_usb narrows it to. cpu1_pingpong_ipc is the only app in
        // the tree that compiles NONE of it. See CrossApp.no_nsc for why the
        // mistake is silent rather than a compile failure, which was measured
        // on all ten units at this app's own bar and contradicts the reason
        // its CMakeLists gives.
        //
        // It is also the first app that is dual-core AND TrustZone at once,
        // and the pair is what makes the CPU1 rules observable rather than
        // assumed. The M33 image carries -DRA8_FREESTANDING, which is
        // directory scope, and NOT -DRA8_TRUSTZONE_ENABLE and NOT -mcmse,
        // which ride on the ra8_add_app() target this hand-rolled executable
        // never is. Its include path is also one directory shorter than
        // cpu1_pingpong's: four dirs, stopping before the board layer, which
        // is why cpu1_image.Cpu1Image now carries that arm as data (#1146).
        //
        // 192 units: blink_hal's universal 200, minus the ten NSC units,
        // plus ra8_tz_secure_boot from LIBS, plus src/ns_main.c. That last
        // one is the app-local glob from #1036 taking an arm nothing else in
        // the table takes -- the app keeps a THIRD unit under its own src/
        // and names only src/cpu1_main.c in AUX_SRCS, so ns_main.c belongs to
        // the M85 image even though its name says otherwise. src/system_init.c
        // and src/trustzone_init.c override two board boot units, the same
        // pair tz_nsc_cgc_usb overrides.
        //
        // It links no migrated Zig archive, so #948 does not block it.
        .name = "cpu1_pingpong_ipc",
        .dir = "examples/ek_ra8d2/hil_needs_revalidation/cpu1_pingpong_ipc",
        .board = "libs/ra8_board_ek_ra8d2",
        // Its own script: .cpu1_image is pinned at ORIGIN(MRAM_CPU1) and the
        // Non-Secure world's load home is carved out besides.
        .linker_script = "examples/ek_ra8d2/hil_needs_revalidation/cpu1_pingpong_ipc/linker_script.ld",
        .libraries = &.{"ra8_tz_secure_boot"},
        .zig_libraries = &.{},
        .aux_srcs = &.{"src/cpu1_main.c"},
        .no_nsc = true,
        .trust_zone = true,
        // The M33 half. Same four units and the same hand-rolled target shape
        // as cpu1_pingpong (#1044), with the board include directory absent:
        // this app's cpu1_main.c reaches core and HAL headers only.
        .cpu1 = .{
            .entry_source = "src/cpu1_main.c",
            .shared_sources = &.{
                "libs/ra8_hal/src/ra8_ipc.c",
                "libs/ra8_core/src/ra8_log.c",
                "libs/ra8_core/src/ra8_scb.c",
            },
            .linker_script = "linker_script_cpu1.ld",
            .board_include_dir = false,
        },
    },
};
