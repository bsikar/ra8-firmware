//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_sd_tour_m85 (RA8FW-1023, under RA8FW-289): CPU0 reads the signed
//! syscall-tour module `txm_tour_m33.ra8app` off the micro-SD card
//! (card.zig), admits it against the pinned key and reads its manifest
//! (verify.zig), and runs it through the ThreadX Module Manager (module.zig)
//! with the resident-image half of `ra8_app_api_t` (backend.zig): the
//! image's `_txm_module_manager_application_request` serves every call the
//! module makes through the checked dispatch, one logged line each, the
//! module exits with the pass, and the unload gives the module pool back
//! whole. Zig throughout; ra8_fs is the existing C library.

const std = @import("std");
const backend = @import("backend.zig");
const card = @import("card.zig");
const module = @import("module.zig");
const verify = @import("verify.zig");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
pub const tamper_line = "txm_sd_tour_m85: tampered image refused PASS\r\n";
pub const pass_line = "txm_sd_tour_m85: tour PASS\r\n";
pub const stack_bytes = 4096;
pub const priority: u32 = 1;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;

var image: [card.file_max]u8 align(8) = undefined;
var thread: [256]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;
/// The admitted file: its payload (preamble first) and the manifest the
/// backend serves the module's calls with.
var loaded: anyerror!App = error.unread;

const App = struct {
    payload: []const u8,
    app_id: []const u8,
    capabilities: u32,
};

extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
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

fn fail(err: anyerror) void {
    var line: [64]u8 = undefined;
    const text = std.fmt.bufPrint(&line, "txm_sd_tour_m85: FAIL {s}\r\n", .{@errorName(err)}) catch return;
    backend.say(text);
}

/// Reads and admits the tour module before the kernel starts, proves the
/// gate refuses a copy with one payload byte flipped (RA8FW-831), and reads
/// the manifest the backend serves the module's calls with.
fn admit() anyerror!App {
    const len = try card.readApp(card.tour_name, &image);
    if (!verify.admitted(image[0..len])) return error.signature;
    if (!verify.refusesTamper(image[0..len])) return error.tamper;
    backend.say(tamper_line);
    const man = verify.manifest(image[0..len]) orelse return error.manifest;
    return .{
        .payload = image[verify.header_bytes..len],
        .app_id = man.app_id,
        .capabilities = man.capabilities,
    };
}

fn run() anyerror!void {
    const app = try loaded;
    try module.init();
    backend.setManifest(app.app_id, app.capabilities);
    const code = try module.tour("txm_tour_m33", app.payload);
    if (code != 0) {
        var line: [64]u8 = undefined;
        const text = std.fmt.bufPrint(&line, "txm_sd_tour_m85: FAIL tour step {d}\r\n", .{code}) catch return;
        backend.say(text);
        return error.tour;
    }
    backend.say(pass_line);
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    if (loaded) |app| {
        run() catch |err| fail(err);
    } else |err| fail(err);
    while (true) _ = _tx_thread_sleep(module.tick_limit);
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    const created = _tx_thread_create(&thread, "tour host", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
    if (created != tx_success) backend.say("txm_sd_tour_m85: FAIL thread\r\n");
}

export fn main() callconv(.c) c_int {
    // CGC first: tx_initialize_low_level.S programs SysTick off the core clock.
    if (ra8_cgc_init() != 0) park();
    _ = ra8_board_uart_console_init(baud);
    loaded = admit();
    _tx_initialize_kernel_enter();
    backend.say("txm_sd_tour_m85: FAIL kernel\r\n");
    park();
    return 0;
}
