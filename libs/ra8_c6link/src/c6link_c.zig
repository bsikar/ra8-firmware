//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one translate-c view of the public `ra8_c6link.h`, shared by every ABI
//! file that reads `ra8_c6link_t`. build.zig translates it once as `c6link_h`,
//! and every ABI file reaches it through here.

/// The public header: the handle, its transport, counters and constants.
pub const c = @import("c6link_h");

/// `ra8_c6link_rx_view_t`: where a classified frame's payload is.
pub const RxView = extern struct {
    offset: u16,
    len: u16,
    if_type: u8,
    if_num: u8,
};

/// protobuf-c's `ProtobufCBinaryData`: a length and a pointer, in that order.
pub const BinaryData = extern struct {
    len: usize,
    data: ?[*]const u8,
};
