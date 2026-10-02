//! A hello-world ThreadX module for CPU1, the RA8D2's Cortex-M33 (RA8FW-430,
//! under RA8FW-290). Upstream's preamble names `demo_module_start` as the
//! module's start thread; the Module Manager runs it as a module thread once
//! the shell entry has set up the GOT and the kernel dispatcher.
//!
//! It keeps no writable globals, so nothing here depends on GOT handling for
//! Zig code: the only reach outside the function is a PC-relative call into
//! libtxm_m33.a. It sleeps one tick at a time forever, so the manager can see
//! it run through the thread's run count.

/// Ticks per sleep: one, so the thread wakes on every kernel tick.
const sleep_ticks: u32 = 1;

/// The module-side sleep in libtxm_m33.a, which traps into the manager.
extern fn _tx_thread_sleep(timer_ticks: u32) u32;

/// The module's start thread, entered with the module's ID.
export fn demo_module_start(id: u32) callconv(.C) noreturn {
    _ = id;
    while (true) _ = _tx_thread_sleep(sleep_ticks);
}
