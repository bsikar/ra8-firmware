//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_touch_led_m85 (RA8FW-1262): legible touch-to-LED demo via a ThreadX
//! module on EK-RA8D2. Carries the signed `txm_touch_led_m33.ra8app` in MRAM
//! (`.txm_module`), verifies its capability admission (`display | led`),
//! opens GT911 touch, loads the module, forwards real touch frames into
//! `touch_event`, drives LED1 (`ra8_board_led_on`) via `led_set`, and
//! verifies clean unload.

const std = @import("std");
const backend = @import("backend.zig");
const module = @import("module.zig");
const verify = @import("verify.zig");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
pub const led1_mask: u32 = 0x1;
pub const cap_refusal_line = "txm_touch_led_m85: withheld led refused ok\r\n";
pub const tamper_line = "txm_touch_led_m85: tampered image refused ok\r\n";
pub const ready_line = "txm_touch_led_m85: waiting for touch\r\n";
pub const pass_line = "txm_touch_led_m85: PASS\r\n";
pub const stack_bytes = 4096;
pub const priority: u32 = 1;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;
const touch_max_points: u8 = 5;
const touch_irq_unset: u8 = 32;
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

const BringupCfg = extern struct {
    console_baud: u32 = 0,
    leds_mask: u32 = 0,
    enable_interrupts: bool = false,
};

const BringupOut = extern struct {
    cpuclk0_hz: u32 = 0,
    pclka_hz: u32 = 0,
};

const BoardTouchCfg = extern struct {
    max_points: u8 = touch_max_points,
    irq_pin: u8 = touch_irq_unset,
};

extern const g_ra8_ls_txm_module_start: u8;
extern const g_ra8_ls_txm_module_end: u8;

extern fn ra8_board_bringup(cfg: *const BringupCfg, out: *BringupOut) callconv(.c) u16;
extern fn ra8_board_touch_open(cfg: *const BoardTouchCfg) callconv(.c) u16;
extern fn ra8_io_i2c_bus_bind_i3c_compat(bus: *anyopaque, channel: u8) callconv(.c) u16;
extern fn ra8_io_i2c_bus_as_ops(bus: *anyopaque, out: *anyopaque) callconv(.c) u16;
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
    const text = std.fmt.bufPrint(&line, "txm_touch_led_m85: FAIL {s}\r\n", .{@errorName(err)}) catch return;
    backend.say(text);
}

fn admit() anyerror!App {
    const start = @intFromPtr(&g_ra8_ls_txm_module_start);
    const end = @intFromPtr(&g_ra8_ls_txm_module_end);
    const len = end - start;
    if (len == 0 or len > blob_max) return error.blob;
    @memcpy(image[0..len], @as([*]const u8, @ptrCast(&g_ra8_ls_txm_module_start))[0..len]);
    if (!verify.refusesWithoutLed(image[0..len])) return error.capability_gate;
    backend.say(cap_refusal_line);
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
    std.mem.doNotOptimizeAway(&ra8_io_i2c_bus_bind_i3c_compat);
    std.mem.doNotOptimizeAway(&ra8_io_i2c_bus_as_ops);
    const touch_cfg: BoardTouchCfg = .{};
    const touch_err = ra8_board_touch_open(&touch_cfg);
    if (touch_err != 0) {
        var eline: [64]u8 = undefined;
        const etext = std.fmt.bufPrint(&eline, "txm_touch_led_m85: touch_open err=0x{x}\r\n", .{touch_err}) catch "";
        if (etext.len > 0) backend.say(etext);
        return error.touch_open;
    }
    try module.init();
    backend.setManifest(app.app_id, app.capabilities);
    backend.say(ready_line);
    const code = try module.runTouchLed("txm_touch_led_m33", app.payload);
    if (code != 0) {
        var line: [64]u8 = undefined;
        const text = std.fmt.bufPrint(&line, "txm_touch_led_m85: FAIL step {d}\r\n", .{code}) catch return;
        backend.say(text);
        return error.module;
    }
    backend.say(pass_line);
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    if (loaded) |_| {
        run() catch |err| fail(err);
    } else |err| fail(err);
    while (true) _ = _tx_thread_sleep(module.idle_sleep_ticks);
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    const created = _tx_thread_create(&thread, "touch led host", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
    if (created != tx_success) backend.say("txm_touch_led_m85: FAIL thread\r\n");
}

export fn main() callconv(.c) c_int {
    const cfg: BringupCfg = .{
        .console_baud = baud,
        .leds_mask = led1_mask,
        .enable_interrupts = false,
    };
    var out: BringupOut = .{};
    if (ra8_board_bringup(&cfg, &out) != 0) park();
    loaded = admit();
    _tx_initialize_kernel_enter();
    backend.say("txm_touch_led_m85: FAIL kernel\r\n");
    park();
    return 0;
}
