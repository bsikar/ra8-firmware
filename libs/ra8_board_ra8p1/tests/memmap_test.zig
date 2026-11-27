//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host consistency tests pinning `libs/ra8_board_ra8p1` linker scripts, CPU1
//! CMake map, `inc/ra8_board_memmap.h`, and `src/boot/system_init.c` to the
//! reconciled RA8P1 cache and TCM geometry (RA8FW-336).

const std = @import("std");

fn readLocal(path: []const u8) ![]u8 {
    return std.Io.Dir.cwd().readFileAlloc(std.testing.io, path, std.testing.allocator, .limited(1 << 16));
}

fn expectAssigned(text: []const u8, sym: []const u8, val: []const u8) !void {
    var pos: usize = 0;
    while (std.mem.indexOfPos(u8, text, pos, sym)) |idx| {
        const after_sym = text[idx + sym.len ..];
        const trimmed = std.mem.trimStart(u8, after_sym, " \t");
        if (std.mem.startsWith(u8, trimmed, "=")) {
            const rhs = std.mem.trimStart(u8, trimmed[1..], " \t");
            try std.testing.expect(std.mem.startsWith(u8, rhs, val));
            return;
        }
        pos = idx + sym.len;
    }
    return error.MissingSymbol;
}

test "RA8P1 linker script keeps 64K TCM floor and single-precision FPU header" {
    const ld = try readLocal("ld/linker_script.ld");
    defer std.testing.allocator.free(ld);

    try std.testing.expect(std.mem.indexOf(u8, ld, "double-precision FPU") == null);
    try std.testing.expect(std.mem.indexOf(u8, ld, "single-precision FPU, fpv5-sp-d16") != null);
    try std.testing.expect(std.mem.indexOf(u8, ld, "ITCM (rwx) : ORIGIN = 0x00000000, LENGTH = 64K") != null);
    try std.testing.expect(std.mem.indexOf(u8, ld, "DTCM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K") != null);
    try std.testing.expect(std.mem.indexOf(u8, ld, "128 KiB ECC capacity") != null);
    try std.testing.expect(std.mem.indexOf(u8, ld, "SRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 1024K - 256") != null);
    try std.testing.expect(std.mem.indexOf(u8, ld, "NOINIT (rw) : ORIGIN = 0x220FFF00, LENGTH = 256") != null);
    try std.testing.expect(std.mem.indexOf(u8, ld, "NS_SRAM (rwx) : ORIGIN = 0x32100000, LENGTH = 640K") != null);
    try std.testing.expect(std.mem.indexOf(u8, ld, "SDRAM (rwx) : ORIGIN = 0x68000000, LENGTH = 64M") != null);
}

test "RA8P1 CPU1 linker script and CMake map record M33 TCM and bus caches" {
    const cpu1_ld = try readLocal("ld/linker_script_cpu1.ld");
    defer std.testing.allocator.free(cpu1_ld);
    const cpu1_cmake = try readLocal("ld/cpu1_memory_map.cmake");
    defer std.testing.allocator.free(cpu1_cmake);

    try std.testing.expect(std.mem.indexOf(u8, cpu1_ld, "MRAM_CPU1 (rx)  : ORIGIN = 0x020C0000, LENGTH = 256K") != null);
    try std.testing.expect(std.mem.indexOf(u8, cpu1_ld, "SRAM_CPU1 (rwx) : ORIGIN = 0x22190000, LENGTH = 64K") != null);
    try std.testing.expect(std.mem.indexOf(u8, cpu1_ld, "64 KiB CTCM + 64 KiB STCM") != null);
    try std.testing.expect(std.mem.indexOf(u8, cpu1_ld, "16 KiB C-Cache + 16 KiB S-Cache") != null);

    try std.testing.expect(std.mem.indexOf(u8, cpu1_cmake, "set(RA8_CPU1_IMAGE_ORIGIN 0x020C0000)") != null);
    try std.testing.expect(std.mem.indexOf(u8, cpu1_cmake, "set(RA8_CPU1_IMAGE_LENGTH 256K)") != null);
    try std.testing.expect(std.mem.indexOf(u8, cpu1_cmake, "set(RA8_CPU1_SRAM_ORIGIN 0x22190000)") != null);
    try std.testing.expect(std.mem.indexOf(u8, cpu1_cmake, "set(RA8_CPU1_SRAM_LENGTH 64K)") != null);
    try std.testing.expect(std.mem.indexOf(u8, cpu1_cmake, "64 KiB CTCM + 64 KiB STCM") != null);
}

test "RA8P1 board memmap and limits headers publish reconciled geometry constants" {
    const memmap = try readLocal("inc/ra8_board_memmap.h");
    defer std.testing.allocator.free(memmap);
    const board_hdr = try readLocal("inc/ra8_board_ra8p1.h");
    defer std.testing.allocator.free(board_hdr);

    try expectAssigned(memmap, "k_ra8_board_itcm_size", "0x00010000UL");
    try expectAssigned(memmap, "k_ra8_board_dtcm_size", "0x00010000UL");
    try expectAssigned(memmap, "k_ra8_board_sram_size", "0x000FFF00UL");
    try expectAssigned(memmap, "k_ra8_board_noinit_size", "0x00000100UL");
    try expectAssigned(memmap, "k_ra8_board_ns_sram_size", "0x000A0000UL");

    try expectAssigned(board_hdr, "k_ra8p1_board_cap_m85_itcm_bytes", "0x00020000UL");
    try expectAssigned(board_hdr, "k_ra8p1_board_cap_m85_dtcm_bytes", "0x00020000UL");
    try expectAssigned(board_hdr, "k_ra8p1_board_cap_m85_icache_bytes", "0x00004000UL");
    try expectAssigned(board_hdr, "k_ra8p1_board_cap_m85_dcache_bytes", "0x00004000UL");
    try expectAssigned(board_hdr, "k_ra8p1_board_cap_m33_ctcm_bytes", "0x00010000UL");
    try expectAssigned(board_hdr, "k_ra8p1_board_cap_m33_stcm_bytes", "0x00010000UL");
    try expectAssigned(board_hdr, "k_ra8p1_board_cap_m33_ccache_bytes", "0x00004000UL");
    try expectAssigned(board_hdr, "k_ra8p1_board_cap_m33_scache_bytes", "0x00004000UL");
    try expectAssigned(board_hdr, "k_ra8p1_board_limit_m85_tcm_floor", "0x00010000UL");
}

test "RA8P1 system_init removes stale cacheless M33 claim and cites HUM geometry" {
    const sys_init = try readLocal("src/boot/system_init.c");
    defer std.testing.allocator.free(sys_init);

    try std.testing.expect(std.mem.indexOf(u8, sys_init, "M33 is cacheless") == null);
    try std.testing.expect(std.mem.indexOf(u8, sys_init, "16 KiB C-Cache + 16 KiB S-Cache") != null);
    try std.testing.expect(std.mem.indexOf(u8, sys_init, "16 KiB I-cache + 16 KiB D-cache") != null);
}

test "RA8P1 concurrent workload header pins memory tiers and headroom" {
    const workloads = try readLocal("inc/ra8_board_ra8p1_workloads.h");
    defer std.testing.allocator.free(workloads);

    try expectAssigned(workloads, "k_ra8p1_mem_m85_itcm_floor_bytes", "0x00010000UL");
    try expectAssigned(workloads, "k_ra8p1_mem_m85_dtcm_floor_bytes", "0x00010000UL");
    try expectAssigned(workloads, "k_ra8p1_mem_sram0_limit_bytes", "0x000FFF00UL");
    try expectAssigned(workloads, "k_ra8p1_mem_noinit_bytes", "0x00000100UL");
    try expectAssigned(workloads, "k_ra8p1_mem_sram1_limit_bytes", "0x000A0000UL");
    try expectAssigned(workloads, "k_ra8p1_mem_sdram_ek_base_bytes", "0x04000000UL");
    try expectAssigned(workloads, "k_ra8p1_mem_sdram_max_cap_bytes", "0x08000000UL");
    try expectAssigned(workloads, "k_ra8p1_itcm_allocated_bytes", "0x00008000UL");
    try expectAssigned(workloads, "k_ra8p1_dtcm_allocated_bytes", "0x00008000UL");
    try expectAssigned(workloads, "k_ra8p1_sram1_allocated_bytes", "0x000A0000UL");
    try expectAssigned(workloads, "k_ra8p1_sdram_allocated_bytes", "0x04000000UL");
}
