//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared vocabulary for the URL and peer-address safety policy: the house
//! error codes this library returns, the published address classes, and the
//! numeric bounds the parsers work to. No logic lives here.

/// House error codes, mirroring `ra8_err.h` (`enum : uint16_t`).
pub const err = struct {
    pub const ok: u16 = 0;
    pub const no_mem: u16 = 0x102;
    pub const invalid_arg: u16 = 0x103;
    pub const not_found: u16 = 0x106;
};

/// How fetchable a peer address is, mirroring `ra8_net_addr_class_t`.
pub const AddrClass = enum(u8) {
    public = 0,
    loopback = 1,
    private = 2,
    linklocal = 3,
    unknown = 4,
};

/// Sizes the parsers and the authority copy work to.
pub const limits = struct {
    /// Authority buffer a caller must reserve: name, port, NUL.
    pub const host_cap: u16 = 262;
    /// Octets in an IPv4 address.
    pub const v4_bytes: usize = 4;
    /// Decimal digits one octet may take.
    pub const v4_digits_max: usize = 3;
    /// Largest value one octet holds.
    pub const v4_octet_max: u32 = 255;
    /// Bytes in an IPv6 address.
    pub const v6_bytes: usize = 16;
    /// 16-bit groups in an IPv6 address.
    pub const v6_groups: usize = 8;
    /// Hex digits one group may carry.
    pub const v6_group_hex: usize = 4;
    /// Offset of the embedded IPv4 address in a mapped literal.
    pub const v6_mapped_v4: usize = 12;
};
