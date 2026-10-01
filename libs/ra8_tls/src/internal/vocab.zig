//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The words this facade speaks: the `ra8_err_t` subset it returns, the
//! static pool bounds, the network constants behind the MSS clamp, and the
//! peer-verification policy `ra8_tls.h` publishes. Nothing here knows what
//! Mbed TLS is.

/// `ra8_err_t` values this library can return (libs/ra8_core/inc/ra8_err.h).
pub const Err = struct {
    pub const ok: u16 = 0;
    pub const no_mem: u16 = 0x102;
    pub const invalid_arg: u16 = 0x103;
    pub const would_block: u16 = 0x10B;
    pub const exists: u16 = 0x10C;
    pub const not_initialized: u16 = 0x10F;
    pub const hw_init_failed: u16 = 0x201;
    pub const hw_error: u16 = 0x204;
    pub const comm_error: u16 = 0x401;
};

/// `ra8_tls_limits_t`. NASA Power of 10 Rule 3: every bound is static.
pub const Limits = struct {
    pub const max_sessions: usize = 4;
    pub const cipher_name_cap: usize = 48;
};

/// `ra8_tls_net_const_t`.
pub const Net = struct {
    pub const ipv4_hdr_bytes: u16 = 20;
    pub const tcp_hdr_bytes: u16 = 20;
    pub const mtu_min: u16 = 128;
    pub const mss_min: u16 = 64;
};

/// `ra8_tls_verify_mode_t`. Open so a value off the wire lands in the C's
/// `default:` arm rather than triggering illegal-value behaviour.
pub const VerifyMode = enum(u8) {
    default = 0,
    none = 1,
    optional = 2,
    required = 3,
    _,
};
