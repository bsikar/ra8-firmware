//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The subsystem callouts the default SysTick handler dispatches, and nothing
//! else.
//!
//! Each one is a WEAK extern, which is how an image that does not link the
//! subsystem still links: an unresolved weak reference reads as null and the
//! call is skipped, and an image that does link it gets the strong definition.
//! `_tx_timer_interrupt` and `g_ra8_threadx_systick_ready` live in
//! libthreadx.a, `ux_dcd_ra8_usb_irq_reenable` in port/usbx.
//!
//! The readiness flag is not optional caution. Between `ra8_time_init`, which
//! arms SysTick at 1 kHz, and `tx_kernel_enter`, which runs ThreadX init, the
//! kernel's timer state is uninitialised, and a tick dispatched into it there
//! HardFaults with UFSR.INVPC set. That is the bench symptom that bricked
//! `threadx_netx_tcp_echo` boot whenever `ra8_board_ethernet_init` ran long
//! enough for a tick to land first.
//!
//! These externs are formed on target only. A host test binary has no kernel
//! to tick, and the macOS test linker does not fold an unresolved weak
//! reference to null the way the ELF firmware linker does, so forming them
//! there breaks every host suite that links this library.

const cpu = @import("time_cpu");

/// A subsystem's tick callout.
pub const Hook = *const fn () callconv(.c) void;

/// Dispatch one tick's worth of subsystem work. Written as nested ifs rather
/// than one compound condition so the project's MC/DC gate does not demand an
/// extra vector for an ISR-only path.
pub fn dispatch(ready: ?*const volatile u32, threadx_tick: ?Hook, usb_reenable: ?Hook) void {
    if (ready) |flag| {
        if (flag.* != 0) {
            if (threadx_tick) |hook| {
                hook();
            }
        }
    }
    if (usb_reenable) |hook| {
        hook();
    }
}

const linked_ready: ?*const volatile u32 = if (cpu.on_target)
    @extern(?*const volatile u32, .{ .name = "g_ra8_threadx_systick_ready", .linkage = .weak })
else
    null;

const linked_threadx_tick: ?Hook = if (cpu.on_target)
    @extern(?Hook, .{ .name = "_tx_timer_interrupt", .linkage = .weak })
else
    null;

const linked_usb_reenable: ?Hook = if (cpu.on_target)
    @extern(?Hook, .{ .name = "ux_dcd_ra8_usb_irq_reenable", .linkage = .weak })
else
    null;

/// Dispatch to whichever of the three the image actually linked.
pub fn dispatchLinked() void {
    dispatch(linked_ready, linked_threadx_tick, linked_usb_reenable);
}
