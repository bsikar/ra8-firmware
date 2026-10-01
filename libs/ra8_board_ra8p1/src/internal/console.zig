//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Debug-UART console policy: the init ordering, the PCLKA floor, the
//! initialized gate, and the bounded polled drain. Generic over the SCI seam so
//! the policy is exercised on the host without an MCU.
//!
//! The console sits on SCI8 with PD02 -> TXD8 and PD03 -> RXD8 (provisional,
//! mirrored from the EK-RA8D2 J-Link OB VCOM). SCI_B's baud generator is
//! clocked from PCLKA (chip HUM R01UH1064EJ Ch 38).

/// Re-exported so a test rooted at this file reaches the shared vocabulary
/// without pulling `vocab.zig` into a second module of the same binary.
pub const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Pin = vocab.Pin;

/// Fixed console wiring (`ra8_board_uart_*` enums in the public header).
pub const Wiring = struct {
    pub const sci_channel: u8 = 8;
    pub const pin_txd: u16 = Pin.pack(13, 2);
    pub const pin_rxd: u16 = Pin.pack(13, 3);
    pub const pin_rts: u16 = Pin.pack(13, 4);
    pub const pin_cts: u16 = Pin.pack(13, 5);
    pub const psel_sci_async: u8 = 0x04;
    pub const clock_id_pclka: u8 = 3;

    /// Before `ra8_cgc_init()` brings the PLL up the chip sits on MOCO
    /// (~8 MHz), far too low for a usable 115200 divisor. This floor sits above
    /// MOCO and below any sane post-PLL PCLKA.
    pub const min_pclka_hz: u32 = 16_000_000;
};

/// SCI framing the console asks for: 8N1 at the caller's baud.
pub const Framing = struct {
    pub const data_8: u8 = 8;
    pub const parity_none: u8 = 0;
    pub const stop_1: u8 = 0;
};

/// Console over a seam that supplies the clock read, the PFS routes and the
/// SCI channel. `Sci` must expose `pclkaHz`, `route`, `init`, `writePolling`,
/// `getc` and `flush`, each returning an `ra8_err_t` code.
pub fn Console(comptime Sci: type) type {
    return struct {
        const Self = @This();

        initialized: bool = false,

        /// Bring the console up: read PCLKA, refuse a pre-PLL clock, route the
        /// pins, then configure the channel. The gate only opens once the
        /// channel is configured.
        pub fn init(self: *Self, baud: u32) u32 {
            if (baud == 0) return Err.invalid_arg;

            // Read PCLKA at runtime so the BRR tracks whatever CGC tree the
            // application settled on, instead of an assumed constant (the
            // classic 2x-too-fast UART bug).
            var pclka_hz: u32 = 0;
            const clock_err = Sci.pclkaHz(&pclka_hz);
            if (clock_err != Err.ok) return clock_err;
            if (pclka_hz < Wiring.min_pclka_hz) return Err.not_initialized;

            const txd_err = Sci.route(Wiring.pin_txd, Wiring.psel_sci_async, "ra8_board.uart.console.txd");
            if (txd_err != Err.ok) return txd_err;
            const rxd_err = Sci.route(Wiring.pin_rxd, Wiring.psel_sci_async, "ra8_board.uart.console.rxd");
            if (rxd_err != Err.ok) return rxd_err;

            const init_err = Sci.init(Wiring.sci_channel, baud, pclka_hz);
            if (init_err != Err.ok) return init_err;

            self.initialized = true;
            return Err.ok;
        }

        /// Blocking write. An empty write is a no-op, checked before the buffer
        /// so a null/zero pair is accepted the way the C entry point was.
        pub fn write(self: *const Self, data: ?[*]const u8, len: usize) u32 {
            if (len == 0) return Err.ok;
            const bytes = data orelse return Err.invalid_arg;
            if (!self.initialized) return Err.not_initialized;
            return Sci.writePolling(Wiring.sci_channel, bytes[0..len]);
        }

        /// Non-blocking drain: pull bytes while the channel has them, stop the
        /// moment it reports none. `cap` bounds the loop (NASA Rule 2).
        pub fn read(self: *const Self, out: ?[*]u8, cap: usize, out_len: ?*usize) u32 {
            const written = out_len orelse return Err.invalid_arg;
            written.* = 0;
            if (cap == 0) return Err.ok;
            const buf = out orelse return Err.invalid_arg;
            if (!self.initialized) return Err.not_initialized;

            for (buf[0..cap]) |*slot| {
                var byte: u8 = 0;
                if (Sci.getc(Wiring.sci_channel, &byte) != Err.ok) return Err.ok;
                slot.* = byte;
                written.* += 1;
            }
            return Err.ok;
        }

        /// Spin until the channel has drained, so a panic handler can get its
        /// failure log out before WFI gates the SCI clock.
        pub fn flush(self: *const Self) u32 {
            if (!self.initialized) return Err.not_initialized;
            return Sci.flush(Wiring.sci_channel);
        }
    };
}
