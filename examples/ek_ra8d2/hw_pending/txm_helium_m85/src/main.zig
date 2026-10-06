//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_helium_m85 (RA8FW-821, under RA8FW-428): the M85 Module Manager
//! starts txm_helium_m85, whose start thread (priority 1) spins checking its
//! own Q0-Q7 and VPR.P0. This manager thread runs at priority 0, so every
//! tick it preempts the module, and each time it sleeps the scheduler
//! resumes the module through its exception return.
//!
//! Each wake the kernel side loads its own pattern into S0-S31 and P0, which
//! takes over every register the module uses, sleeps one tick in the same
//! asm block, and checks S16-S31 (Q4-Q7) afterwards. Q0-Q3 and VPR are
//! caller-saved under AAPCS, so only the module, which never calls anything,
//! can hold them across a switch; the module side checks all of them. On a
//! wrong value the module sleeps forever, so its run count stops rising.
//!
//! PASS: `switches` wakes where the module ran in between (its run count
//! rose), with the kernel's Q4-Q7 intact on every wake and the module still
//! ready. A wake where the count did not rise is not a failure: the tick can
//! land before the scheduler resumes the module, so that wake just does not
//! count. The manager gives up after `tick_limit` wakes.

const std = @import("std");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
/// Consecutive good switches that count as a pass.
pub const switches: u32 = 10;
/// Ticks the manager waits for the module to start, and for the switches.
pub const tick_limit: u32 = 1000;
pub const pass_line = std.fmt.comptimePrint("txm_helium_m85: Q0-Q7 and VPR kept across {d} switches PASS\r\n", .{switches});
pub const fail_load = "txm_helium_m85: FAIL load\r\n";
pub const fail_kernel = "txm_helium_m85: FAIL kernel Q4-Q7\r\n";
pub const fail_module = "txm_helium_m85: FAIL module stopped\r\n";

/// S0's value on the kernel side; S{n} holds kernel_base + n. The module
/// uses 0x4D50_0000.
pub const kernel_base: u32 = 0x4B45_0000;
/// VPR.P0 while the kernel thread runs. The module uses 0x5A5A.
pub const kernel_p0: u32 = 0xA5A5;
pub const lanes = 32;
/// S16, the first callee-saved lane (Q4).
pub const first_saved_lane = 16;

/// Sizes as txm_manager_m85 measured them on the same port.
pub const thread_bytes = 256;
pub const instance_bytes = 1280;
/// offsetof(TXM_MODULE_INSTANCE, txm_module_instance_start_stop_thread) is
/// 0xC0 in the M85 archive, read off a run (the thread switched to sits at
/// main.instance + 0xC0 and carries TX_THREAD_ID). 0xD0 there is the
/// thread's stack end, so a count read at 0xD4 is the constant stack size.
pub const start_thread_offset = 0xC0;
/// TX_THREAD_ID, the first word of a created TX_THREAD.
pub const tx_thread_id: u32 = 0x5448_5244;
pub const start_run_count_offset = start_thread_offset + 4;
/// offsetof(TX_THREAD, tx_thread_state) is 48 on the same port (no
/// TX_THREAD_EXTENSION_0): 2 terminated (a module fault), 4 sleeping (the
/// module saw a wrong value).
pub const start_state_offset = start_thread_offset + 48;
/// TX_READY: the module thread is runnable (not sleeping or terminated).
pub const tx_ready: u32 = 0;
pub const stack_bytes = 2048;
pub const module_ram_bytes = 16 * 1024;
pub const object_pool_bytes = 4 * 1024;
/// Above the module's start thread, which the preamble puts at 1.
pub const priority: u32 = 0;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;

var thread: [thread_bytes]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;
var instance: [instance_bytes]u8 align(8) = undefined;
var module_ram: [module_ram_bytes]u8 align(32) = undefined;
var object_pool: [object_pool_bytes]u8 align(8) = undefined;

extern const g_ra8_ls_txm_module_start: u8;

extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn _tx_initialize_kernel_enter() void;
extern fn _tx_thread_create(
    thread_ptr: *anyopaque,
    name: [*:0]const u8,
    entry: *const fn (u32) callconv(.c) void,
    input: u32,
    stack_start: *anyopaque,
    stack_size: u32,
    prio: u32,
    preempt_threshold: u32,
    time_slice: u32,
    start: u32,
) callconv(.c) u32;
extern fn _tx_thread_sleep(ticks: u32) callconv(.c) u32;
extern fn _tx_time_get() callconv(.c) u32;
extern fn _txm_module_manager_initialize(ram: *anyopaque, size: u32) callconv(.c) u32;
extern fn _txm_module_manager_object_pool_create(pool: *anyopaque, size: u32) callconv(.c) u32;
extern fn _txm_module_manager_in_place_load(module: *anyopaque, name: [*:0]const u8, location: *const anyopaque) callconv(.c) u32;
extern fn _txm_module_manager_start(module: *anyopaque) callconv(.c) u32;

const set_base = std.fmt.comptimePrint("movw r1, #{d}\nmovt r1, #{d}\n", .{ kernel_base & 0xFFFF, kernel_base >> 16 });

/// S{n} = kernel_base + n for every lane, then P0.
const load_lanes = blk: {
    var text: []const u8 = set_base;
    for (0..lanes) |n| text = text ++ std.fmt.comptimePrint("vmov s{d}, r1\nadds r1, r1, #1\n", .{n});
    break :blk text ++ std.fmt.comptimePrint("movw r1, #{d}\n", .{kernel_p0}) ++ vmsr_p0_r1;
};

/// %[bad] = 1 on the first of S16-S31 that is not kernel_base + n.
const check_saved = blk: {
    var text: []const u8 = std.fmt.comptimePrint("movw r1, #{d}\nmovt r1, #{d}\n", .{ (kernel_base + first_saved_lane) & 0xFFFF, (kernel_base + first_saved_lane) >> 16 });
    for (first_saved_lane..lanes) |n| text = text ++ std.fmt.comptimePrint("vmov r2, s{d}\ncmp r2, r1\nbne 2f\nadds r1, r1, #1\n", .{n});
    break :blk text;
};

/// The app's Zig is built for an M85 model without the FP features (the C
/// units carry them), so the block turns them on itself.
const features = ".fpu fpv5-sp-d16\n";

/// `vmsr p0, r1`, encoded: this assembler knows no MVE extension to enable.
const vmsr_p0_r1 = ".inst.w 0xEEED1A10\n";

const switch_once = features ++ load_lanes ++ "movs r0, #1\nbl _tx_thread_sleep\n" ++ check_saved ++
    "movs %[bad], #0\nb 3f\n2:\nmovs %[bad], #1\n3:\n";

/// One switch: load the kernel pattern, sleep a tick, check Q4-Q7.
fn kernelKeeps() bool {
    const bad = asm volatile (switch_once
        : [bad] "=&r" (-> u32),
        :
        : "r0", "r1", "r2", "r3", "r12", "lr", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15", "cc", "memory"
    );
    return bad == 0;
}

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

fn loadAndStart() bool {
    if (_txm_module_manager_initialize(&module_ram, module_ram_bytes) != tx_success) return false;
    if (_txm_module_manager_object_pool_create(&object_pool, object_pool_bytes) != tx_success) return false;
    if (_txm_module_manager_in_place_load(&instance, "txm_helium_m85", &g_ra8_ls_txm_module_start) != tx_success) return false;
    if (_txm_module_manager_start(&instance) != tx_success) return false;
    return startWord(start_thread_offset) == tx_thread_id;
}

fn startWord(offset: usize) u32 {
    const word: *align(1) volatile u32 = @ptrCast(&instance[offset]);
    return word.*;
}

fn startRunCount() u32 {
    return startWord(start_run_count_offset);
}

var line_buf: [96]u8 = undefined;

/// fail_module with the start thread's state and run count, so a log says
/// which way the module stopped.
fn moduleStopped(good: u32, wakes: u32) []const u8 {
    const fmt = "txm_helium_m85: FAIL module stopped, state {d}, runs {d}, good {d}, wakes {d}, time {d}\r\n";
    return std.fmt.bufPrint(&line_buf, fmt, .{ startWord(start_state_offset), startRunCount(), good, wakes, _tx_time_get() }) catch fail_module;
}

/// The verdict line: the first failure, else the pass line.
fn verdict() []const u8 {
    if (!loadAndStart()) return fail_load;
    var ticks: u32 = 0;
    while (startRunCount() == 0) : (ticks += 1) {
        if (ticks >= tick_limit) return moduleStopped(0, ticks);
        _ = _tx_thread_sleep(1);
    }
    var good: u32 = 0;
    var wakes: u32 = 0;
    while (good < switches) : (wakes += 1) {
        if (wakes >= tick_limit) return moduleStopped(good, wakes);
        const before = startRunCount();
        if (!kernelKeeps()) return fail_kernel;
        if (startWord(start_state_offset) != tx_ready) return moduleStopped(good, wakes + 1000);
        if (startRunCount() != before) good += 1;
    }
    return pass_line;
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    say(verdict());
    while (true) _ = _tx_thread_sleep(tick_limit);
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    const created = _tx_thread_create(&thread, "module manager", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
    if (created != tx_success) say(fail_load);
}

export fn main() callconv(.c) c_int {
    // CGC first: tx_initialize_low_level.S programs SysTick off the core clock.
    if (ra8_cgc_init() != 0) park();
    _ = ra8_board_uart_console_init(baud);
    _tx_initialize_kernel_enter();
    say(fail_load);
    park();
}
