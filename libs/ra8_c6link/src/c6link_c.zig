//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one translate-c view of the public `ra8_c6link.h`, shared by every ABI
//! file that reads `ra8_c6link_t`. Two separate `@cImport`s would give two
//! unrelated handle types, so they all import this.

/// The public header: the handle, its transport, counters and constants.
pub const c = @cImport({
    @cDefine("static_assert", "_Static_assert");
    @cDefine("alignas", "_Alignas");
    @cInclude("stdbool.h");
    @cInclude("ra8_c6link.h");
});

/// `ra8_c6link_rx_view_t`: where a classified frame's payload is.
pub const RxView = extern struct {
    offset: u16,
    len: u16,
    if_type: u8,
    if_num: u8,
};
