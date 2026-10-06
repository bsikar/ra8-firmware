//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_dual_mailbox, M85 half (RA8FW-843 and RA8FW-844, under RA8EMU-159): a
//! ThreadX module on each core of one image, and calls between them. The M85
//! links `threadx_m85_modules` and carries txm_rpc_m33, the `ra8_rpc` client
//! module, in its own `.txm_module` (linker_append.ld). main clears the
//! mailbox block, releases CPU1 and enters the kernel. The manager thread
//! loads and starts the module, which attaches its two queues through an
//! application request. Once a tick the thread pumps the module's calls into
//! the block's request slot and CPU1's answers from the reply slot into the
//! module's queue (pump.zig). It prints one line once both modules have run
//! `pass_runs` times, and the verdict once the module has reported
//! `pass_calls` checked sums.

const std = @import("std");
const shared = @import("shared.zig");
const service = @import("service.zig");
const pump = @import("pump.zig");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
/// Ticks the manager waits for both modules and the calls before failing.
pub const tick_limit: u32 = 2000;
pub const modules_line = std.fmt.comptimePrint("txm_dual_mailbox: modules ran {d} times on both cores PASS\r\n", .{shared.pass_runs});
pub const pass_line = std.fmt.comptimePrint("txm_dual_mailbox: {d} requests crossed the mailbox PASS\r\n", .{shared.pass_calls});
pub const fail_line = "txm_dual_mailbox: FAIL\r\n";
pub const cpu1_fail_line = "txm_dual_mailbox: FAIL cpu1\r\n";

/// sizeof(TX_THREAD) is 232 on the cortex_m33 module port; headroom.
pub const thread_bytes = 256;
/// sizeof(TXM_MODULE_INSTANCE) is 1196 on the same port and defines.
pub const instance_bytes = 1280;
pub const stack_bytes = 2048;
pub const module_ram_bytes = 16 * 1024;
pub const object_pool_bytes = 4 * 1024;
pub const priority: u32 = 1;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;
const ok: c_int = 0;

var thread: [thread_bytes]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;
var instance: [instance_bytes]u8 align(8) = undefined;
var module_ram: [module_ram_bytes]u8 align(32) = undefined;
var object_pool: [object_pool_bytes]u8 align(8) = undefined;

/// The start of the M85's `.txm_module`, set by linker_append.ld.
extern const g_ra8_ls_txm_module_start: u8;
extern const g_ra8_ls_cpu1_mram_start: u8;
extern const g_ra8_ls_cpu1_stack_top: u8;

extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn ra8_cpu1_release(mram: *const anyopaque, stack: *const anyopaque) c_int;
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
extern fn _txm_module_manager_initialize(ram: *anyopaque, size: u32) callconv(.c) u32;
extern fn _txm_module_manager_object_pool_create(pool: *anyopaque, size: u32) callconv(.c) u32;
extern fn _txm_module_manager_in_place_load(module: *anyopaque, name: [*:0]const u8, location: *const anyopaque) callconv(.c) u32;
extern fn _txm_module_manager_start(module: *anyopaque) callconv(.c) u32;

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
    if (_txm_module_manager_in_place_load(&instance, "txm_rpc_m33", &g_ra8_ls_txm_module_start) != tx_success) return false;
    if (_txm_module_manager_start(&instance) != tx_success) return false;
    return shared.instanceWord(&instance, shared.m85_start_thread_offset) == shared.tx_thread_id;
}

/// The module's two queues once it attached them: the one to this image,
/// then the one back. Written on the module's thread.
var attached: [2]usize = .{ 0, 0 };
/// Sums the module reported, and how many were wrong or out of step.
var reports: u32 = 0;
var mismatches: u32 = 0;
var module_failed: bool = false;

/// The manager's hook for the module's application requests, less
/// `service.Report.base`. It runs on the module's thread, in this image.
export fn _txm_module_manager_application_request(
    request: u32,
    param_1: u32,
    param_2: u32,
    param_3: u32,
) callconv(.c) u32 {
    _ = param_3;
    switch (request) {
        service.Report.attach => {
            attached[1] = param_2;
            @atomicStore(usize, &attached[0], param_1, .release);
        },
        service.Report.value => {
            // param_1 is the sum and param_2 the step it belongs to.
            const step = @atomicLoad(u32, &reports, .monotonic);
            if (step != param_2 or param_1 != service.sumFor(param_2)) _ = @atomicRmw(u32, &mismatches, .Add, 1, .release);
            @atomicStore(u32, &reports, step + 1, .release);
        },
        service.Report.failed => @atomicStore(bool, &module_failed, true, .release),
        else => _ = @atomicRmw(u32, &mismatches, .Add, 1, .release),
    }
    return tx_success;
}

/// Move whatever can move between the module's queues and the block.
fn pumpOnce(block: *volatile shared.Block) void {
    const up = @atomicLoad(usize, &attached[0], .acquire);
    if (up == 0) return;
    const down: *anyopaque = @ptrFromInt(attached[1]);
    while (pump.send(@ptrFromInt(up), &block.request) or pump.take(&block.reply, down)) {}
}

/// Both modules' run counts against `pass_runs`, CPU1's off the block.
fn modulesRan(block: *volatile shared.Block) bool {
    const m85_runs = shared.instanceWord(&instance, shared.m85_start_thread_offset + shared.run_count_offset);
    if (m85_runs < shared.pass_runs) return false;
    return block.signature == shared.signature and block.module_runs >= shared.pass_runs;
}

const Verdict = enum { waiting, pass, failed, cpu1_failed };

fn verdict(block: *volatile shared.Block) Verdict {
    if (block.signature == shared.signature and block.failed_step != shared.Step.none) return .cpu1_failed;
    if (@atomicLoad(bool, &module_failed, .acquire)) return .failed;
    if (@atomicLoad(u32, &mismatches, .acquire) != 0) return .failed;
    if (!modulesRan(block)) return .waiting;
    if (@atomicLoad(u32, &reports, .acquire) < shared.pass_calls) return .waiting;
    return .pass;
}

fn waitAll(block: *volatile shared.Block) []const u8 {
    var said_modules = false;
    var ticks: u32 = 0;
    while (ticks < tick_limit) : (ticks += 1) {
        pumpOnce(block);
        if (!said_modules and modulesRan(block)) {
            say(modules_line);
            said_modules = true;
        }
        switch (verdict(block)) {
            .waiting => _ = _tx_thread_sleep(1),
            .pass => return pass_line,
            .failed => return fail_line,
            .cpu1_failed => return cpu1_fail_line,
        }
    }
    return fail_line;
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    const block = shared.block();
    say(if (loadAndStart()) waitAll(block) else fail_line);
    // Keep the calls moving after the verdict, so neither side times out.
    while (true) {
        pumpOnce(block);
        _ = _tx_thread_sleep(1);
    }
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    const created = _tx_thread_create(&thread, "module manager", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
    if (created != tx_success) say(fail_line);
}

export fn main() callconv(.c) c_int {
    const block = shared.block();
    block.* = .{};
    asm volatile ("dsb" ::: "memory");
    // CGC first: tx_initialize_low_level.S programs SysTick off the core clock.
    if (ra8_cgc_init() != ok) park();
    _ = ra8_board_uart_console_init(baud);
    if (ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top) != ok) {
        say(cpu1_fail_line);
        park();
    }
    _tx_initialize_kernel_enter();
    say(fail_line);
    park();
}
