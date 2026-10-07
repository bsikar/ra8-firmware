//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! cpu1_routed_irq, CPU1 half (RA8FW-809). Links the routed GPT0 overflow to
//! its ICU line 0, enables NVIC IRQ 0, reports armed, then sleeps. The handler
//! clears the latch, disables the line so it runs once, and reports. This
//! image has no .data or .bss, so the reset path copies nothing.

const shared = @import("shared.zig");

const nvic_iser0: usize = 0xE000_E100;
const nvic_icer0: usize = 0xE000_E180;
const line0: u32 = 1;
const vector_count = 112;
const irq0_vector = 16;

extern const g_ra8_ls_cpu1_stack_top: u8;

fn reg(address: usize) *volatile u32 {
    return @ptrFromInt(address);
}

fn fault() callconv(.c) noreturn {
    while (true) asm volatile ("nop");
}

fn gpt0Handler() callconv(.c) void {
    reg(shared.cpu1_ielsr0).* &= ~shared.ielsr_ir;
    reg(nvic_icer0).* = line0;
    shared.block().irq_count = 1;
    asm volatile ("dsb" ::: .{ .memory = true });
}

export fn cpu1_reset_handler() callconv(.c) noreturn {
    reg(shared.cpu1_ielsr0).* = shared.event;
    reg(nvic_iser0).* = line0;
    shared.block().armed = 1;
    asm volatile ("dsb" ::: .{ .memory = true });
    while (true) asm volatile ("wfi");
}

export const g_cpu1_vector_table: [vector_count]*const anyopaque linksection(".cpu1_vectors") = table: {
    var vectors: [vector_count]*const anyopaque = undefined;
    for (&vectors) |*vector| vector.* = &fault;
    vectors[0] = &g_ra8_ls_cpu1_stack_top;
    vectors[1] = &cpu1_reset_handler;
    vectors[irq0_vector] = &gpt0Handler;
    break :table vectors;
};
