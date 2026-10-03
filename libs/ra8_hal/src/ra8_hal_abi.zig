//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports of the ported ra8_hal units. The prototypes in inc/ are
//! unchanged and stay the membrane: callers cannot tell which side of the
//! port a symbol is on.

const eth_media = @import("internal/eth_media.zig");

extern fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// `ra8_err_t` values (libs/ra8_core/inc/ra8_err.h).
const k_ra8_ok: u16 = 0;
const k_ra8_err_invalid_arg: u16 = 0x103;

const eth_tag = "ETH";

/// `ra8_err_t ra8_eth_rgmii_select(ra8_eth_mii_port_t port)` (inc/ra8_eth.h).
export fn ra8_eth_rgmii_select(port: u8) u16 {
    eth_media.rgmiiSelect(eth_media.hardware(), port) catch {
        ra8_log_emit_error(eth_tag, "Range check failed");
        return k_ra8_err_invalid_arg;
    };
    ra8_log_emit_info(eth_tag, "rgmii_select");
    return k_ra8_ok;
}
