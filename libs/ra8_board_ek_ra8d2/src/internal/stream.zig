//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Layout mirrors of the two `ra8_io` types this board layer holds by value.
//! Both are deliberately small and stable; the board owns the storage, the
//! sink owns what goes in it.

/// `ra8_io_stream_t`: a bound sink vtable plus its context.
pub const IoStream = extern struct {
    iface: ?*const anyopaque = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_io_stream_uart_state_t`: the sink's private SCI channel.
pub const UartState = extern struct {
    channel: u8 = 0,
};
