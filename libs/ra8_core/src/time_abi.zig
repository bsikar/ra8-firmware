//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_time.h`.
//!
//! The millisecond timebase sits on the SysTick and DWT primitive ported in
//! This file owns the tick counter, the delay policy and the SysTick
//! IRQ body, and every raw register access stays behind
//! `ra8_systick_configure` / `ra8_dwt_cyccnt_*`.
//!
//! EVERY ENTRY POINT HERE IS WEAK, and only one of them was weak in the C.
//!
//! `SysTick_Handler` was: sixteen example apps publish a strong handler of
//! their own and must keep winning over this default, and the board's vector
//! table carries a weak alias to `Default_Handler` for an image that links
//! neither.
//!
//! The other three are weak because a composed archive has no way to decline
//! a TU. The C excluded this file from an image by not compiling it, which is
//! what `tz_nsc_cgc_usb`'s NS half does: its `ra8_time_init` would reprogram
//! the SysTick ThreadX owns, and its delay would wait on a tick the ThreadX
//! handler never advances, so `ns_usb.c` defines a ThreadX-backed
//! `ra8_delay_ms` and `ra8_time_ms` instead. Since the archive fix a freestanding image
//! links ONE ra8_core archive, so that image now receives this TU whether it
//! wants it or not, and a strong export here would make its own definitions
//! duplicate symbols. Weak linkage is the same override by another spelling:
//! an image that defines one wins, an image that does not gets this default.
//!
//! The log lines go through `ra8_log_emit_*`, and the DEMCR unlock behind
//! `ra8_dwt_cyccnt_enable` reaches ra8_scb, Zig as of the fault block.

const reload_math = @import("time_reload");
const tick = @import("time_tick");
const delay = @import("time_delay");
const hooks = @import("time_hooks");
const cpu = @import("time_cpu");

/// `ra8_err_t` values this module returns, from `inc/ra8_err.h`.
const err = struct {
    pub const ok: c_int = 0;
    pub const invalid_arg: c_int = 0x103;
};

/// `ra8_systick_clock_source_t` values, from `inc/ra8_systick.h`.
const clock_source = struct {
    pub const cpu: u8 = 1;
};

const tag: [*:0]const u8 = "TIME";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_info_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;
extern fn ra8_systick_configure(reload: u32, src: u8, tick_irq: bool) c_int;
extern fn ra8_dwt_cyccnt_enable() void;
extern fn ra8_dwt_cyccnt_read() u32;

/// Arm the timebase: SysTick at 1 kHz off the CPU clock, and the cycle counter
/// the masked-interrupt delay falls back on.
fn arm(reload: u32) ?c_int {
    if (!cpu.on_target) return null;

    const code = ra8_systick_configure(reload, clock_source.cpu, true);
    if (code != err.ok) {
        ra8_log_emit_error(tag, "systick configure failed");
        return code;
    }
    ra8_dwt_cyccnt_enable();
    return null;
}

fn timeInit(cpu_hz: u32) callconv(.c) c_int {
    const reload = reload_math.reloadFor(cpu_hz) catch |e| switch (e) {
        error.ZeroClock => {
            ra8_log_emit_error(tag, "cpu_hz must be non-zero");
            return err.invalid_arg;
        },
        error.ClockTooLow => {
            ra8_log_emit_error(tag, "cpu_hz too low for 1kHz tick");
            return err.invalid_arg;
        },
    };

    if (arm(reload)) |code| return code;

    tick.setCyclesPerMs(reload_math.cyclesPerMs(cpu_hz));
    tick.reset();
    ra8_log_emit_info_val(tag, "systick reload", reload);
    return err.ok;
}

fn timeMs() callconv(.c) u32 {
    return tick.now();
}

fn delayMs(ms: u32) callconv(.c) void {
    delay.wait(&ra8_dwt_cyccnt_read, ms);
}

fn timeOnTick() callconv(.c) void {
    tick.advance();
}

/// The default SysTick IRQ body: advance the counter, then hand the tick to
/// whichever subsystems the image linked.
fn systickHandler() callconv(.c) void {
    tick.advance();
    if (cpu.on_target) hooks.dispatchLinked();
}

/// Every entry point, paired with the name the header publishes.
const weak_surface = .{
    .{ "ra8_time_init", &timeInit },
    .{ "ra8_time_ms", &timeMs },
    .{ "ra8_delay_ms", &delayMs },
    // The short-form aliases from the deleted libs/ra8_hal/src/timer.c. They
    // share the default bodies, so an image that overrides ra8_time_ms or
    // ra8_delay_ms and also calls these must override them too (no image
    // does today: only tz_nsc_cgc_usb's ns_usb.c overrides, and it never
    // calls them).
    .{ "ra8_now_ms", &timeMs },
    .{ "ra8_sleep_ms", &delayMs },
    .{ "ra8_time_on_tick", &timeOnTick },
    .{ "SysTick_Handler", &systickHandler },
};

comptime {
    for (weak_surface) |entry| {
        @export(entry[1], .{ .name = entry[0], .linkage = .weak });
    }
}
