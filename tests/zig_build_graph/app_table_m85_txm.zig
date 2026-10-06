//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The CPU0 ThreadX Module Manager apps: the M85 links threadx_m85_modules and
//! carries one module in `.txm_module` (RA8FW-796). Split out of app_table.zig
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
};
