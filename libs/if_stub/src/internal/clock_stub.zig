//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The stub clock profile, and nothing else.
//!
//! `if_stub` is the proof binding of epic RA8FW-618 slice 5: the `if_<chip>`
//! set a second target supplies in place of the RA8 adapters, built and
//! host-tested to prove a portable library needs no edit when the chip
//! changes. This unit is that set's clock member. It reads no hardware and
//! touches no register, so the whole profile is a pure function of its
//! arguments and every row can be driven on the host without a fake
//! peripheral block.
//!
//! The profile is deliberately tiny and deliberately arbitrary: three modules
//! (the core, one UART, one SPI host) at fixed rates a stub board carries.
//! The rates assert nothing about real silicon; they exist so the port's rate,
//! floor and gating intents can be exercised end to end through the facade. A
//! real chip port replaces this file, and the interface above it never
//! changes.
//!
//! The kinds are spelled numerically so this unit stays free of the C header;
//! the smoke translation unit pins them against `fw_if_clock.h` itself, which
//! is what keeps the two transcriptions from drifting apart.

/// `fw_clock_module_kind_t` values the table names.
pub const core_kind: u8 = 1; // k_fw_clock_module_core
pub const uart_kind: u8 = 2; // k_fw_clock_module_uart
pub const spi_kind: u8 = 3; // k_fw_clock_module_spi

/// One module instance the stub board carries, with the rate it feeds.
pub const Entry = struct {
    kind: u8,
    index: u8,
    rate_hz: u32,
};

/// The stub profile. Dense board numbering, index zero for each kind.
pub const entries = [_]Entry{
    .{ .kind = core_kind, .index = 0, .rate_hz = 48_000_000 },
    .{ .kind = uart_kind, .index = 0, .rate_hz = 24_000_000 },
    .{ .kind = spi_kind, .index = 0, .rate_hz = 48_000_000 },
};

/// Resolve a neutral module against the stub profile. Returns the rate of the
/// module the board carries, or null when the instance is absent, which the
/// membrane reports as `not_found`.
pub fn resolve(kind: u8, index: u8) ?u32 {
    if (index != 0) return null;

    for (entries) |e| {
        if (e.kind == kind) return e.rate_hz;
    }

    return null;
}
