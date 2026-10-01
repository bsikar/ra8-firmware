//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The TCP MSS clamp behind `ra8_tls_mss_clamp`. Pure arithmetic over the
//! pinned header sizes, so the whole of it is reachable from the host suite.

const vocab = @import("vocab.zig");

const Net = vocab.Net;

/// Bytes an IPv4 + TCP header pair costs, neither carrying options.
pub const overhead: u16 = Net.ipv4_hdr_bytes + Net.tcp_hdr_bytes;

/// Largest segment `mtu` can carry, or null when what is left after the
/// headers would be below `Net.mss_min`.
pub fn clamp(mtu: u16) ?u16 {
    if (mtu <= overhead) return null;
    const mss = mtu - overhead;
    if (mss < Net.mss_min) return null;
    return mss;
}
