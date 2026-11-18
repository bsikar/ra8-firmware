//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_manager_ra8p1 (RA8FW-680, under RA8FW-1186): the ThreadX Module
//! Manager on the RA8P1, carrying the translation module's signed `.ra8app`
//! in its own `.txm_module` in MRAM (linker_append.ld, CrossApp.ra8app_module).
//! No card: the blob is in the image. The manager thread copies it out to
//! SRAM (MRAM stays read-only storage), admits it against the pinned key
//! (verify.zig, with RA8FW-831's tamper refusal), memory-loads it through
//! the Module Manager (the RA8FW-830 pattern), serves the shell's display
//! and storage calls through the resident-image half of ra8_app_api_t
//! (backend.zig, a RAM file store where the card examples use the FAT
//! volume), and reads the exit code the shell asks for: zero is the pass.
//! Zig throughout, so no CMakeLists.

const std = @import("std");
const backend = @import("backend.zig");
const module = @import("module.zig");
const verify = @import("verify.zig");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
pub const tamper_line = "txm_manager_ra8p1: tampered image refused PASS\r\n";
pub const pass_line = "txm_manager_ra8p1: translate shell PASS\r\n";
pub const stack_bytes = 4096;
pub const priority: u32 = 1;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;

/// Headroom for the copied blob: the shell's `.ra8app` is a few KiB.
pub const blob_max = 64 * 1024;

var image: [blob_max]u8 align(8) = undefined;
var thread: [256]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;
var loaded: anyerror!App = error.unread;

const App = struct {
    payload: []const u8,
    app_id: []const u8,
    capabilities: u32,
};

/// `.txm_module` in MRAM, set by this app's linker_append.ld.
extern const g_ra8_ls_txm_module_start: u8;
extern const g_ra8_ls_txm_module_end: u8;

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
    const text = std.fmt.bufPrint(&line, "txm_manager_ra8p1: FAIL {s}\r\n", .{@errorName(err)}) catch return;
    backend.say(text);
}

/// Copies the MRAM blob out to SRAM, admits it, proves the gate refuses a
/// tampered copy (RA8FW-831), and reads the manifest the backend serves the
/// shell's calls with. The copy is what memory_load consumes, so MRAM stays
/// read-only storage the whole way (RA8FW-830's pattern).
fn admit() anyerror!App {
    const start = @intFromPtr(&g_ra8_ls_txm_module_start);
    const end = @intFromPtr(&g_ra8_ls_txm_module_end);
    const len = end - start;
    if (len == 0 or len > blob_max) return error.blob;
    @memcpy(image[0..len], @as([*]const u8, @ptrCast(&g_ra8_ls_txm_module_start))[0..len]);
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
    const code = try module.translate("txm_translate_m33", app.payload);
    if (code != 0) {
        var line: [64]u8 = undefined;
        const text = std.fmt.bufPrint(&line, "txm_manager_ra8p1: FAIL translate step {d}\r\n", .{code}) catch return;
        backend.say(text);
        return error.translate;
    }
    backend.say(pass_line);
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    if (loaded) |_| {
        run() catch |err| fail(err);
    } else |err| fail(err);
    while (true) _ = _tx_thread_sleep(module.tick_limit);
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    const created = _tx_thread_create(&thread, "translate host", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
    if (created != tx_success) backend.say("txm_manager_ra8p1: FAIL thread\r\n");
}

export fn main() callconv(.c) c_int {
    // CGC first: tx_initialize_low_level.S programs SysTick off the core clock.
    if (ra8_cgc_init() != 0) park();
    _ = ra8_board_uart_console_init(baud);
    loaded = admit();
    _tx_initialize_kernel_enter();
    backend.say("txm_manager_ra8p1: FAIL kernel\r\n");
    park();
    return 0;
}
