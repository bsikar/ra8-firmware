//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! cpu1_pingpong_ra8p1, CPU1 half (RA8FW-496). No RTOS, so this file owns the
//! M33's vector table and reset path: copy .data, clear .bss, then answer every
//! ping the M85 posts in the shared block.

const shared = @import("shared.zig");

extern const g_ra8_ls_cpu1_stack_top: u8;
extern var g_ra8_ls_cpu1_data_start: u8;
extern const g_ra8_ls_cpu1_data_end: u8;
extern const g_ra8_ls_cpu1_data_load: u8;
extern var g_ra8_ls_cpu1_bss_start: u8;
extern const g_ra8_ls_cpu1_bss_end: u8;

/// Volatile word loops, so the -nostdlib link never needs memcpy or memset.
fn initMemory() void {
    const data: [*]volatile u32 = @ptrCast(@alignCast(&g_ra8_ls_cpu1_data_start));
    const load: [*]const volatile u32 = @ptrCast(@alignCast(&g_ra8_ls_cpu1_data_load));
    const data_words = (@intFromPtr(&g_ra8_ls_cpu1_data_end) - @intFromPtr(data)) / 4;
    for (0..data_words) |i| data[i] = load[i];
    const bss: [*]volatile u32 = @ptrCast(@alignCast(&g_ra8_ls_cpu1_bss_start));
    const bss_words = (@intFromPtr(&g_ra8_ls_cpu1_bss_end) - @intFromPtr(bss)) / 4;
    for (0..bss_words) |i| bss[i] = 0;
}

/// Answers one ping: a wrong payload still gets an ack so the M85's poll
/// ends, and the M85 counts the mismatch from `pong_payload`.
fn pong(block: *volatile shared.Block, seq: u32) void {
    const good = block.ping_payload == shared.magic_ping;
    block.pong_payload = if (good) shared.magic_pong else 0;
    asm volatile ("dsb" ::: .{ .memory = true });
    block.pong_seq = seq;
}

export fn cpu1_reset_handler() callconv(.c) noreturn {
    initMemory();
    const block = shared.block();
    var seen: u32 = 0;
    while (true) {
        const seq = block.ping_seq;
        if (seq == seen) continue;
        seen = seq;
        pong(block, seq);
    }
}

/// Any fault parks CPU1; the M85 then times out and prints FAIL.
export fn cpu1_fault_handler() callconv(.c) noreturn {
    while (true) asm volatile ("wfi");
}

/// The M33's 16 system vectors, pinned by linker_script_cpu1.ld.
export const _vectors linksection(".cpu1_vectors") = [16]?*const anyopaque{
    &g_ra8_ls_cpu1_stack_top,
    @ptrCast(&cpu1_reset_handler),
    @ptrCast(&cpu1_fault_handler), // NMI
    @ptrCast(&cpu1_fault_handler), // HardFault
    @ptrCast(&cpu1_fault_handler), // MemManage
    @ptrCast(&cpu1_fault_handler), // BusFault
    @ptrCast(&cpu1_fault_handler), // UsageFault
    @ptrCast(&cpu1_fault_handler), // SecureFault
    null,
    null,
    null,
    @ptrCast(&cpu1_fault_handler), // SVCall
    @ptrCast(&cpu1_fault_handler), // DebugMonitor
    null,
    @ptrCast(&cpu1_fault_handler), // PendSV
    @ptrCast(&cpu1_fault_handler), // SysTick
};
