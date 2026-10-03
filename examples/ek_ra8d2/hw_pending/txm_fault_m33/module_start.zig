//! A ThreadX module for CPU1 that breaks its own isolation on purpose
//! (RA8FW-459, under RA8FW-290): the negative case beside txm_hello_m33.
//!
//! Its start thread stores one word to `outside_address`, in shared SRAM and
//! outside every MPU region the Module Manager gives the module. Running
//! unprivileged behind the MPU, that store takes MemManage and the manager
//! terminates the thread, so the sleep loop below is never reached. If it is
//! reached, isolation did not hold, and txm_fault_cpu1's M85 times out on FAIL.
//!
//! Like txm_hello_m33 it keeps no writable globals: the address is a literal
//! and the only call is PC-relative into libtxm_m33.a.

/// Just past txm_fault_cpu1's shared block (0x2210_0000, 32 bytes), so a store
/// a broken MPU lets through cannot fake the block's verdict.
pub const outside_address: usize = 0x2210_0040;
/// "FAUL", the word the module tries to leave there.
pub const poke_value: u32 = 0x4641_554C;

const sleep_ticks: u32 = 1;

/// The module-side sleep in libtxm_m33.a, which traps into the manager.
extern fn _tx_thread_sleep(timer_ticks: u32) u32;

/// The module's start thread, entered with the module's ID.
export fn demo_module_start(id: u32) callconv(.C) noreturn {
    _ = id;
    const target: *volatile u32 = @ptrFromInt(outside_address);
    target.* = poke_value;
    while (true) _ = _tx_thread_sleep(sleep_ticks);
}
