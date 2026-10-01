//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The caller-facing records of `ra8_tls.h`, laid out as the C lays them out.
//! The facade reads these; only `ra8_tls_abi.zig` ever receives one from
//! outside the archive.

const vocab = @import("vocab.zig");

/// `ra8_err_t` as a C function-pointer return type.
pub const Err = u16;

/// `ra8_tls_transport_t`: the seam an application implements so no app ever
/// includes an Mbed TLS header to author a transport.
pub const Transport = extern struct {
    send: ?*const fn (?*anyopaque, ?[*]const u8, usize, ?*usize) callconv(.c) Err = null,
    recv: ?*const fn (?*anyopaque, ?[*]u8, usize, ?*usize) callconv(.c) Err = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_tls_session_cfg_t`.
pub const Session = extern struct {
    transport: Transport = .{},
    server_name: ?[*:0]const u8 = null,
    verify_mode: vocab.VerifyMode = .default,
    ca_pem: ?[*]const u8 = null,
    ca_pem_len: usize = 0,

    /// The trust anchor as a slice, or null when the caller supplied none.
    pub fn caPem(self: *const Session) ?[]const u8 {
        const pem = self.ca_pem orelse return null;
        if (self.ca_pem_len == 0) return null;
        return pem[0..self.ca_pem_len];
    }
};
