//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_eth_rgmii_select (internal/eth_media.zig, RA8FW-497). Built as its own object in
//! libra8_hal.a (RA8FW-542) so an image links only the units it calls.

const common = @import("abi_common.zig");
const eth_media = @import("internal/eth_media.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const ra8_log_emit_info = common.ra8_log_emit_info;
const ra8_log_emit_error = common.ra8_log_emit_error;

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
