//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The J-Link OB VCOM serial bridge (board UM Table 13, p 24): SCI8 out on
//! PD02/PD03, the only console the board has before an application brings up
//! one of its own.
//!
//! The baud generator is fed by PCLKA on RA8D2 (chip HUM Ch 38.2 "SCI
//! registers"), not PCLKB as on the lower-end RA parts, and the rate is read
//! from CGC at init rather than assumed. Assuming it was the bug behind every
//! garbled-UART report from an app that retunes its clock tree: the BRR came
//! out for 60 MHz against a real 125 MHz clock, so the line ran twice too fast.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

/// Where the console sits on this board, and what it needs to work.
pub const Console = struct {
    /// PD02/PD03 route to SCI8, verified on real EK-RA8D2 v1 silicon.
    pub const sci_channel: u8 = 8;
    /// PD02 TXD, chip coordinate P1302.
    pub const pin_txd: u16 = vocab.Pin.pack(13, 2);
    /// PD03 RXD, chip coordinate P1303.
    pub const pin_rxd: u16 = vocab.Pin.pack(13, 3);
    /// Floor for a usable 115200 BRR. Set above MOCO (~8 MHz), which the chip
    /// sits on before CGC runs, and well below the 125 MHz PLL1 target, so a
    /// different clock tree can still bring the console up.
    pub const min_pclka_hz: u32 = 16_000_000;
};

/// True once `init` has configured SCI8 and routed the two pins. The write,
/// read and flush paths refuse while it is false, so no caller can drive an
/// unconfigured channel.
var console_up: bool = false;

/// Whether the console has ever come up. Published as a predicate rather than
/// a mutable flag: a caller can ask, and cannot lie about it.
pub fn isUp() bool {
    return console_up;
}

/// Configure SCI8 and route PD02/PD03 for the console.
pub fn init(baud: u32) u32 {
    if (baud == 0) return vocab.Err.invalid_arg;

    var pclka_hz: u32 = 0;
    const clock_err = hal.ra8_cgc_get_clock_hz(vocab.ClockId.pclka, &pclka_hz);
    if (clock_err != vocab.Err.ok) return clock_err;
    if (pclka_hz < Console.min_pclka_hz) return vocab.Err.not_initialized;

    // PSEL 00100b covers SCI async TXD/RXD per the chip HUM Multiplexed Pin
    // Function Selector.
    const txd_err = hal.ra8_pfs_route_peripheral(
        Console.pin_txd,
        vocab.Psel.sci_async,
        "ra8_board.uart.console.txd",
    );
    if (txd_err != vocab.Err.ok) return txd_err;
    const rxd_err = hal.ra8_pfs_route_peripheral(
        Console.pin_rxd,
        vocab.Psel.sci_async,
        "ra8_board.uart.console.rxd",
    );
    if (rxd_err != vocab.Err.ok) return rxd_err;

    const cfg: hal.SciCfg = .{
        .baud = baud,
        .data_bits = hal.Sci.data_8,
        .parity = hal.Sci.parity_none,
        .stop_bits = hal.Sci.stop_1,
        .pclk_hz = pclka_hz,
    };
    const sci_err = hal.ra8_sci_init(Console.sci_channel, &cfg);
    if (sci_err != vocab.Err.ok) return sci_err;

    console_up = true;
    return vocab.Err.ok;
}

/// Write the whole slice to the console, blocking until the SCI takes it.
pub fn write(data: []const u8) u32 {
    if (!console_up) return vocab.Err.not_initialized;
    return hal.ra8_sci_write_polling(Console.sci_channel, data.ptr, @intCast(data.len));
}

/// Drain whatever the console has, without blocking for more.
///
/// Pulls bytes while the SCI keeps reporting one, and stops the moment it does
/// not. The slice bounds the loop. Returns the filled prefix of `out`; an empty
/// prefix means nothing had arrived, which is not an error.
pub fn read(out: []u8) struct { err: u32, filled: usize } {
    if (!console_up) return .{ .err = vocab.Err.not_initialized, .filled = 0 };
    for (out, 0..) |*slot, i| {
        var byte: u8 = 0;
        if (hal.ra8_sci_getc_polling(Console.sci_channel, &byte) != vocab.Err.ok) {
            return .{ .err = vocab.Err.ok, .filled = i };
        }
        slot.* = byte;
    }
    return .{ .err = vocab.Err.ok, .filled = out.len };
}

/// Spin until the console's transmit path has drained to the wire. Lets a
/// panic path flush its failure log before WFI gates the SCI clock.
pub fn flush() u32 {
    if (!console_up) return vocab.Err.not_initialized;
    return hal.ra8_sci_flush(Console.sci_channel);
}
