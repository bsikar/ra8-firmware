//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A byte-stream handle bound to the J-Link OB VCOM console.
//!
//! The console is a board singleton, one VCOM bridge on one SCI channel, so
//! its sink state is module-owned rather than caller-supplied. That is what
//! lets the entry point take only a handle. Two handles bound here share these
//! bytes and therefore address the same console, which is intended.

const hal = @import("hal.zig");
const stream = @import("stream.zig");
const vocab = @import("vocab.zig");

pub const Err = vocab.Err;
pub const IoStream = stream.IoStream;

/// PD02/PD03 reach SCI8, verified on real EK-RA8D2 v1 silicon.
pub const sci_channel: u8 = 8;

/// Sink state backing every stream handle bound to the board console.
var console_sink: stream.UartState = .{};

/// Bind @p out to the board console.
///
/// Refuses with `not_initialized` until `ra8_board_uart_console_init` has run,
/// because the sink writes straight at an SCI channel that is not yet up, and
/// with `not_supported` when the app did not link `ra8_io` and so has no sink
/// to bind. See `hal.ra8_io_stream_uart_init` for why that is a runtime
/// refusal rather than a link error.
pub fn bind(out: ?*IoStream) u32 {
    return bindThrough(hal.ra8_io_stream_uart_init, out);
}

/// `bind` with the sink passed in, so the absent-sink refusal is testable
/// without a second link.
pub fn bindThrough(sink: ?*const hal.UartStreamInit, out: ?*IoStream) u32 {
    const dst = out orelse return Err.null_ptr;
    const init = sink orelse return Err.not_supported;
    if (!hal.priv_ra8_board_uart_console_is_up()) return Err.not_initialized;
    return init(dst, &console_sink, sci_channel);
}
