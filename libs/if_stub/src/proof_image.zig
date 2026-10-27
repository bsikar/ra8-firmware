//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The slice-5 proof image: the complete `if_stub` set bound through the real
//! `libs/if` facade, linked into one freestanding image (RA8FW-976, epic
//! RA8FW-618 slice 5).
//!
//! Every layer of the port stack is present at once: the four stub profiles,
//! the four stub op membranes, the `fw_*_bind` externs they reach, and the
//! facade that owns those externs. The reset path binds all four interfaces
//! through the stub conveniences and then runs each one back through the
//! facade's dispatch, so a symbol missing from the image is a link error, not
//! a silent hole. The facade is imported rather than redeclared: its export
//! functions are emitted because the proof calls them, and the stub binds
//! resolve against those same exports through the C ABI.
//!
//! This is a proof, not a product surface: it builds only for freestanding
//! targets, it carries its own `_start`, and no consumer links it.

const clock_ops = @import("clock_stub_ops_abi");
const rng_abi = @import("rng_stub_abi");
const gpio_abi = @import("gpio_stub_abi");
const uart_abi = @import("uart_stub_abi");

const fw_if = @import("fw_if");

/// One bit per proof step, set when the step did not hold its contract.
/// Zero is a clean proof. Exported so the result survives optimization and
/// stays readable from the image.
pub var result: u32 = 0;

comptime {
    @export(&result, .{ .name = "ra8_proof_result" });
}

fn fail(comptime step: u5) void {
    result |= @as(u32, 1) << step;
}

const marker = "ra8 stub proof\n";

fn run() void {
    // Clock: bind through the stub convenience, then run the rate and gate
    // paths back through the facade.
    var clk_handle: clock_ops.Handle = .{ .iface = null, .ctx = null, .bound = false };
    if (clock_ops.fw_clock_stub_bind(&clk_handle) != 0) fail(0);
    const clk: ?*const fw_if.clock.Clock = @ptrCast(&clk_handle);

    var hz: u32 = 0;
    if (fw_if.clock.fw_clock_rate_for(clk, .{ .kind = 0, .index = 0 }, &hz) != 0) fail(1);
    if (hz == 0) fail(2);
    if (fw_if.clock.fw_clock_enable(clk, .{ .kind = 0, .index = 0 }) != 0) fail(3);
    if (fw_if.clock.fw_clock_disable(clk, .{ .kind = 0, .index = 0 }) != 0) fail(4);

    // Random bytes: bind, then take the deterministic pattern through the
    // facade and confirm the binding reports itself ready.
    var rng_handle: rng_abi.Handle = .{ .iface = null, .ctx = null, .bound = false };
    if (rng_abi.fw_rng_stub_bind(&rng_handle) != 0) fail(5);
    const rng: ?*const fw_if.rng.Rng = @ptrCast(&rng_handle);
    var bytes: [8]u8 = undefined;
    if (fw_if.rng.fw_rng_bytes(rng, &bytes, bytes.len) != 0) fail(6);
    var ready: bool = false;
    if (fw_if.rng.fw_rng_ready(rng, &ready) != 0) fail(7);
    if (!ready) fail(8);

    // GPIO: bind, put pin 0 into a driven output, and read the drive back
    // through the facade. Direction 2 is fw_gpio_output, pull 0 is
    // fw_gpio_pull_none.
    var gpio_handle: gpio_abi.Handle = .{ .iface = null, .ctx = null, .bound = false };
    if (gpio_abi.fw_gpio_stub_bind(&gpio_handle) != 0) fail(9);
    const gpio: ?*const fw_if.gpio.Gpio = @ptrCast(&gpio_handle);
    if (fw_if.gpio.fw_gpio_configure(gpio, 0, .{ .direction = 2, .pull = 0 }) != 0) fail(10);
    if (fw_if.gpio.fw_gpio_write(gpio, 0, true) != 0) fail(11);
    var level: bool = false;
    if (fw_if.gpio.fw_gpio_read(gpio, 0, &level) != 0) fail(12);
    if (!level) fail(13);

    // UART: bind, say so on the console line, and read silence off the
    // modem line, which nothing has primed.
    var uart_handle: uart_abi.Handle = .{ .iface = null, .ctx = null, .bound = false };
    if (uart_abi.fw_uart_stub_bind(&uart_handle) != 0) fail(14);
    const uart: ?*const fw_if.uart.Uart = @ptrCast(&uart_handle);
    if (fw_if.uart.fw_uart_write(uart, 0, marker, marker.len) != 0) fail(15);
    var echo: [8]u8 = undefined;
    var count: usize = 0;
    if (fw_if.uart.fw_uart_read(uart, 1, &echo, echo.len, &count) != 0) fail(16);
    if (count != 0) fail(17);
}

export fn _start() callconv(.c) noreturn {
    run();

    while (true) {}
}
