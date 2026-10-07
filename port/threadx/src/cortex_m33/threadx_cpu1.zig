//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ThreadX on CPU1, the RA8D2's Cortex-M33 (RA8FW-409).
//!
//! The upstream `ports/cortex_m33/gnu/src/tx_initialize_low_level.S` kept in
//! the `threadx_m33` archive reads three things the CPU1 image has to give it:
//! `_vectors` (the reset SP and VTOR come from it), `__RAM_segment_used_end__`
//! (the first free byte, a `--defsym` in `cpu1_threadx.zig`) and a SysTick
//! reload, which it hard-codes for a 6 MHz clock. This module is the first
//! two plus the reset path; `startTicks` fixes the third.
//!
//! A CPU1 app whose Zig entry names `uses = threadx_m33` imports this module
//! as `threadx_cpu1` and exports `tx_application_define` itself. It must call
//! `startTicks` there with CPU1's clock, so the kernel ticks at
//! `ticks_per_second` (tx_user.h's TX_TIMER_TICKS_PER_SECOND).

/// MemManage, BusFault and SVC differ per kernel: the build graph imports
/// `cpu1_handlers_single.zig` or `cpu1_handlers_modules.zig` here.
const handlers = @import("cpu1_handlers");

pub const ticks_per_second: u32 = 1000;

/// SysTick and SCB registers the reset path and `startTicks` write.
pub const reg = struct {
    pub const syst_csr: usize = 0xE000_E010;
    pub const syst_rvr: usize = 0xE000_E014;
    pub const syst_cvr: usize = 0xE000_E018;
    pub const cpacr: usize = 0xE000_ED88;
};

/// SYST_CSR: enable, interrupt on wrap, processor clock.
pub const syst_csr_run: u32 = 0x7;
/// CPACR: full access to CP10 and CP11 (the FPU).
pub const cpacr_fpu: u32 = 0xF << 20;

extern var g_ra8_ls_cpu1_stack_top: u8;
extern var g_ra8_ls_cpu1_data_start: u8;
extern var g_ra8_ls_cpu1_data_end: u8;
extern const g_ra8_ls_cpu1_data_load: u8;
extern var g_ra8_ls_cpu1_bss_start: u8;
extern var g_ra8_ls_cpu1_bss_end: u8;

extern fn _tx_initialize_kernel_enter() callconv(.c) void;
extern fn __tx_NMIHandler() callconv(.c) void;
extern fn __tx_BadHandler() callconv(.c) void;
extern fn __tx_DBGHandler() callconv(.c) void;
extern fn HardFault_Handler() callconv(.c) void;
extern fn UsageFault_Handler() callconv(.c) void;
extern fn PendSV_Handler() callconv(.c) void;
extern fn SysTick_Handler() callconv(.c) void;

/// The SysTick reload for `cpu_hz`, or null when it does not fit 24 bits or
/// the clock is below one tick.
pub fn reload(cpu_hz: u32) ?u32 {
    const cycles = cpu_hz / ticks_per_second;
    if (cycles == 0 or cycles > 0x0100_0000) return null;
    return cycles - 1;
}

/// Points SysTick at `ticks_per_second` for a core running at `cpu_hz`.
/// Call it from `tx_application_define`, after the port's own setup ran.
pub fn startTicks(cpu_hz: u32) void {
    const value = reload(cpu_hz) orelse return;
    write(reg.syst_csr, 0);
    write(reg.syst_rvr, value);
    write(reg.syst_cvr, 0);
    write(reg.syst_csr, syst_csr_run);
}

fn write(address: usize, value: u32) void {
    @as(*volatile u32, @ptrFromInt(address)).* = value;
}

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

export fn cpu1_reset_handler() callconv(.c) noreturn {
    initMemory();
    const cpacr: *volatile u32 = @ptrFromInt(reg.cpacr);
    cpacr.* |= cpacr_fpu;
    asm volatile ("dsb\n\tisb" ::: .{ .memory = true });
    _tx_initialize_kernel_enter();
    while (true) asm volatile ("wfi");
}

/// The M33's 16 system vectors. `_vectors` is the name the port reads.
export const _vectors linksection(".cpu1_vectors") = [16]?*const anyopaque{
    &g_ra8_ls_cpu1_stack_top,
    @ptrCast(&cpu1_reset_handler),
    @ptrCast(&__tx_NMIHandler),
    @ptrCast(&HardFault_Handler),
    @ptrCast(handlers.mem_manage),
    @ptrCast(handlers.bus_fault),
    @ptrCast(&UsageFault_Handler),
    @ptrCast(&__tx_BadHandler), // SecureFault
    null,
    null,
    null,
    @ptrCast(handlers.svc),
    @ptrCast(&__tx_DBGHandler),
    null,
    @ptrCast(&PendSV_Handler),
    @ptrCast(&SysTick_Handler),
};
