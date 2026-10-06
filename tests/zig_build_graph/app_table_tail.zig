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
};
