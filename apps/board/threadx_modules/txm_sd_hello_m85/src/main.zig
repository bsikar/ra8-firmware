//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_sd_hello_m85 (RA8FW-829 and RA8FW-830, under RA8FW-290): CPU0 reads
//! the signed hello-world module `txm_hello_m33.ra8app` off the micro-SD card
//! (card.zig), admits it against the pinned key and checks a tampered copy
//! is refused (verify.zig, RA8FW-831), and runs it
//! unprivileged through the ThreadX Module Manager (module.zig): memory load,
//! start, ten runs of its start thread, stop, unload, and the same again from
//! the same image, with the module pool back to its size after each unload
//! (RA8FW-1022). Then the negative
//! (RA8FW-837): the signed `txm_fault_m33.ra8app` off the same card stores
//! outside its MPU regions, the manager's fault callback sees it killed, and
//! the manager ticks on. The verdicts go to the SCI8 VCOM console. Zig
//! throughout; ra8_fs is the existing C library.

const std = @import("std");
const card = @import("card.zig");
const verify = @import("verify.zig");
const module = @import("module.zig");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
pub const pass_line = "txm_sd_hello_m85: signed module loaded, ran, exited PASS\r\n";
pub const tamper_line = "txm_sd_hello_m85: tampered image refused PASS\r\n";
pub const fault_line = "txm_sd_hello_m85: faulting module killed, firmware ran on PASS\r\n";
pub const reload_line = "txm_sd_hello_m85: unloaded and reloaded the same image PASS\r\n";
pub const next_line = "txm_sd_hello_m85: next app loaded and ran after the kill PASS\r\n";
/// Load-run-unload cycles of the hello module from one image (RA8FW-1022).
pub const hello_cycles = 2;
pub const stack_bytes = 4096;
pub const priority: u32 = 1;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;

var image: [card.file_max]u8 align(8) = undefined;
var fault_image: [card.file_max]u8 align(8) = undefined;
/// The admitted files' lengths, or the step that refused one.
var loaded: anyerror!Lengths = error.unread;

const Lengths = struct { hello: usize, fault: usize };

var thread: [256]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;

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

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn fail(err: anyerror) void {
    var line: [64]u8 = undefined;
    const text = std.fmt.bufPrint(&line, "txm_sd_hello_m85: FAIL {s}\r\n", .{@errorName(err)});
    say(text catch "txm_sd_hello_m85: FAIL\r\n");
}

/// Reads and admits both modules before the kernel starts, and proves the
/// gate refuses a copy of the hello module with one payload byte flipped
/// (RA8FW-831).
fn admit() anyerror!Lengths {
    const len = try card.readApp(card.hello_name, &image);
    if (!verify.admitted(image[0..len])) return error.signature;
    if (!verify.refusesTamper(image[0..len])) return error.tamper;
    say(tamper_line);
    const fault_len = try card.readApp(card.fault_name, &fault_image);
    if (!verify.admitted(fault_image[0..fault_len])) return error.fault_signature;
    return .{ .hello = len, .fault = fault_len };
}

/// One line per hello cycle: the run, and what its unload gave back.
fn sayCycle(round: u32, cycle: module.Cycle) void {
    var line: [128]u8 = undefined;
    const text = std.fmt.bufPrint(&line, "txm_sd_hello_m85: run {d} exited, unload returned {d} bytes, module pool {d} of {d} free\r\n", .{ round, cycle.held(), cycle.after, cycle.before }) catch return;
    say(text);
}

/// The isolation demo's evidence (RA8FW-1024): where the module stored, the
/// fault that stopped it, and where in the module the store was.
fn sayKill(kill: module.Kill) void {
    var line: [128]u8 = undefined;
    const text = std.fmt.bufPrint(&line, "txm_sd_hello_m85: fault module stored to 0x{X:0>8} from pc 0x{X:0>8}, MPU killed it (CFSR 0x{X:0>8})\r\n", .{ kill.address, kill.pc, kill.status }) catch return;
    say(text);
}

fn runModules(lens: Lengths) anyerror!void {
    try module.init();
    var round: u32 = 1;
    while (round <= hello_cycles) : (round += 1) {
        sayCycle(round, try module.hello(image[verify.header_bytes..lens.hello]));
    }
    say(reload_line);
    say(pass_line);
    sayKill(try module.fault(fault_image[verify.header_bytes..lens.fault]));
    _ = try module.hello(image[verify.header_bytes..lens.hello]);
    say(next_line);
    say(fault_line);
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    if (loaded) |lens| {
        runModules(lens) catch |err| fail(err);
    } else |err| fail(err);
    while (true) _ = _tx_thread_sleep(module.tick_limit);
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    const created = _tx_thread_create(&thread, "module manager", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
    if (created != tx_success) say("txm_sd_hello_m85: FAIL thread\r\n");
}

export fn main() callconv(.c) c_int {
    // CGC first: tx_initialize_low_level.S programs SysTick off the core clock.
    if (ra8_cgc_init() != 0) park();
    _ = ra8_board_uart_console_init(baud);
    loaded = admit();
    _tx_initialize_kernel_enter();
    say("txm_sd_hello_m85: FAIL kernel\r\n");
    park();
}
