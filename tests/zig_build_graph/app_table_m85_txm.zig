//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The CPU0 ThreadX module apps: the M85 links threadx_m85_modules and carries
//! one module in `.txm_module` (RA8FW-796), plus the SD hello-world PoC that
//! reads its module off the card (RA8FW-829). Split out of app_table.zig
//! to keep that file under the line limit (RA8FW-805); app_table appends these
//! last, so the positional picks stay put.

const CrossApp = @import("app_table.zig").CrossApp;

pub const apps = [_]CrossApp{
    .{
        // The ThreadX Module Manager on CPU0: the M85 loads and runs the
        // hello-world module itself, no CPU1 image (RA8FW-795). Zig
        // throughout, so no CMakeLists. Appended last so the positional
        // picks stay put.
        .name = "txm_manager_m85",
        .dir = "examples/ek_ra8d2/hw_pending/txm_manager_m85",
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{},
        .zig_libraries = &.{},
        .uses = &.{"threadx"},
        .threadx_heap = "SDRAM",
        .zig_main = "src/main.zig",
        .txm_module = "txm_hello_m33",
    },
    .{
        // The negative case on CPU0: the M85's Module Manager starts
        // txm_fault_m33, whose store outside its MPU regions must kill only
        // the module (RA8FW-805).
        .name = "txm_fault_m85",
        .dir = "examples/ek_ra8d2/hw_pending/txm_fault_m85",
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{},
        .zig_libraries = &.{},
        .uses = &.{"threadx"},
        .threadx_heap = "SDRAM",
        .zig_main = "src/main.zig",
        .txm_module = "txm_fault_m33",
    },
    .{
        // Helium state across a kernel/module switch on CPU0 (RA8FW-821,
        // under RA8FW-428): the manager thread holds its own Q0-Q7 and VPR
        // while txm_helium_m85, built for the M85 with MVE, spins checking
        // its own.
        .name = "txm_helium_m85",
        .dir = "examples/ek_ra8d2/hw_pending/txm_helium_m85",
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{},
        .zig_libraries = &.{},
        .uses = &.{"threadx"},
        .threadx_heap = "SDRAM",
        .zig_main = "src/main.zig",
        .txm_module = "txm_helium_m85",
    },
    .{
        // Slice 1 of the SD hello-world PoC (RA8FW-829, under RA8FW-290):
        // the M85 mounts the micro-SD card on SDHI0 through ra8_fs and reads
        // and header-checks txm_hello_m33.ra8app. No ThreadX yet; RA8FW-830
        // adds the Module Manager load.
        .name = "txm_sd_hello_m85",
        .dir = "examples/ek_ra8d2/hw_pending/txm_sd_hello_m85",
        .board = "libs/ra8_board_ek_ra8d2",
        .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
        .libraries = &.{"ra8_fs"},
        .zig_libraries = &.{},
        .zig_main = "src/main.zig",
        .stack_bytes = 8192,
    },
};
