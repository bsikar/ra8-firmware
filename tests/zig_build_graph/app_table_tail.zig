//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Cross-build applications added after app_table.zig reached its 1000-line
//! ceiling (RA8FW-846). Data only, in declaration order; app_table appends
//! these after the M85 TXM apps so the positional picks stay put.

const CrossApp = @import("app_table.zig").CrossApp;

pub const apps = [_]CrossApp{
    .{
        // RA8FW-809: CPU0 routes GPT0's overflow (event 0xC1) to CPU1 through
        // INTSELR; CPU1's IRQ handler records it in shared SRAM and CPU0
        // prints the verdict. Zig on both cores, so no CMakeLists.
        .name = "cpu1_routed_irq",
        .dir = "examples/ek_ra8d2/hw_pending/cpu1_routed_irq",
        .cpu1_image = true,
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{"ra8_hal"},
        .zig_libraries = &.{"ra8_hal"},
        .zig_main = "src/main.zig",
        .cpu1 = .{
            .entry_source = "src/cpu1_main.zig",
            .shared_sources = &.{},
            .linker_script = "linker_script_cpu1.ld",
            .entry_language = .zig,
        },
    },
    .{
        // RA8FW-624: two ThreadX workers keep distinct S0-S31 values live
        // across real M85 PendSV switches with lazy stacking on. Zig main
        // (its switch routine is module-level asm), ThreadX only.
        .name = "threadx_fpu_context",
        .dir = "examples/ek_ra8d2/hil_needs_revalidation/threadx_fpu_context",
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{},
        .zig_libraries = &.{},
        .uses = &.{"threadx"},
        .threadx_heap = "SDRAM",
        .zig_main = "src/main.zig",
    },
    .{
        // RA8FW-353: immutable 128 KiB USB-DFU MRAM bootloader linking ThreadX,
        // USBX, ra8_dfu_device, ra8_dfu, ra8_psa_crypto, ra8_rot_launch, ra8_rot,
        // and the trimmed p256-m TF-PSA-Crypto archive.
        .name = "dfu_bootloader",
        .dir = "examples/ek_ra8d2/hw_validated/hil/dfu_bootloader",
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{ "ra8_dfu_device", "ra8_dfu", "ra8_psa_crypto", "ra8_rot_launch", "ra8_rot" },
        .zig_libraries = &.{},
        .min_build_type = .relwithdebinfo,
        .stack_bytes = 32768,
        .uses = &.{ "threadx", "usbx" },
        .threadx_heap = "SDRAM",
        .mram_length = "128K",
        .sram_text = &.{"ra8_flash.c"},
        .local = .{
            .defines = &.{"-DRA8_ENABLE_ROOT_OF_TRUST"},
            .include_dirs = &.{
                "libs/ra8_psa_crypto/inc",
                "libs/ra8_dfu/inc",
            },
            .vendored = .{
                .name = "tfpsa_dfu",
                .source_dirs = &.{
                    "pkg:tf_psa_crypto/core",
                    "pkg:tf_psa_crypto/drivers/builtin/src",
                    "pkg:tf_psa_crypto/drivers/p256-m",
                    "pkg:tf_psa_crypto/drivers/p256-m/p256-m",
                    "pkg:tf_psa_crypto/platform",
                    "pkg:tf_psa_crypto/utilities",
                    "pkg:tf_psa_crypto/extras",
                },
                .system_include_dirs = &.{
                    "examples/ek_ra8d2/hw_validated/hil/dfu_bootloader/inc",
                    "pkg:tf_psa_crypto/include",
                    "pkg:tf_psa_crypto/drivers/builtin/include",
                    "pkg:tf_psa_crypto/drivers/p256-m",
                    "pkg:tf_psa_crypto/drivers/p256-m/p256-m",
                    "pkg:tf_psa_crypto/core",
                    "pkg:tf_psa_crypto/dispatch",
                    "pkg:tf_psa_crypto/drivers/builtin/src",
                    "pkg:tf_psa_crypto/platform",
                    "pkg:tf_psa_crypto/utilities",
                    "pkg:tf_psa_crypto/extras",
                    "pkg:mbedtls/include",
                    "port/mbedtls/inc",
                },
                .defines = &.{
                    "-DMBEDTLS_CONFIG_FILE=\"mbedtls_config.h\"",
                    "-DTF_PSA_CRYPTO_CONFIG_FILE=\"tf_psa_crypto_config_dfu.h\"",
                    "-DMBEDTLS_PLATFORM_MEMORY",
                    "-DMBEDTLS_MEMORY_BUFFER_ALLOC_C",
                },
                .compile_options = &.{"-fno-strict-aliasing"},
            },
        },
    },
    .{
        // Bidirectional M85 <-> M33 ra8_rpc calls, events, and coordinated reset
        // over the shared-SRAM ring transport and IPC FIFO doorbells with
        // active MPU/D-cache mapping shared SRAM Normal Non-cacheable.
        .name = "rpc_ring_dualcore",
        .dir = "examples/ek_ra8d2/hw_pending/rpc_ring_dualcore",
        .cpu1_image = true,
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{"ra8_hal"},
        .zig_libraries = &.{"ra8_hal"},
        .cache_mpu = true,
        .zig_main = "src/main.zig",
        .zig_main_imports = &.{
            .{ .name = "ra8_rpc", .dep = "ra8_rpc", .module = "ra8_rpc" },
        },
        .cpu1 = .{
            .entry_source = "src/cpu1_main.zig",
            .shared_sources = &.{},
            .linker_script = "linker_script_cpu1.ld",
            .entry_language = .zig,
            .zig_imports = &.{
                .{ .name = "ra8_rpc", .dep = "ra8_rpc", .module = "ra8_rpc" },
            },
        },
    },
    .{
        // RA8FW-997: C client of the ra8_box Zig C ABI executing shared
        // vectors from tests/zig_abi_pairs/ra8_box_pair/vectors.json.
        .name = "ra8_box_pair_c",
        .dir = "examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_c",
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{"ra8_box"},
        .zig_libraries = &.{"ra8_box"},
        .stack_bytes = 4096,
        .extra_srcs = &.{"tests/zig_abi_pairs/ra8_box_pair/src/ra8_box_pair_runner.c"},
        .local = .{
            .include_dirs = &.{
                "libs/ra8_ui/inc",
                "tests/zig_abi_pairs/ra8_box_pair/inc",
            },
        },
    },
};
