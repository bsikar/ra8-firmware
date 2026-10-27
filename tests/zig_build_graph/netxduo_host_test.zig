//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The netxduo_host slice's own rules (RA8FW-1065): the tx_port.h rewrite is
//! the one `__x86_64__` ULONG line widened, and the slice's directory lists
//! are named, so a stale package fetch cannot compile an empty stack quietly.

const std = @import("std");
const graph = @import("build_graph");

const host = graph.netxduo_host;

test "tx_port.h rewrite widens the one __x86_64__ ULONG line" {
    try std.testing.expectEqual(@as(usize, 1), host.tx_port_header.rewrites.len);
    try std.testing.expect(std.mem.endsWith(u8, host.tx_port_header.header, "tx_port.h"));
    const rewrite = host.tx_port_header.rewrites[0];
    try std.testing.expect(std.mem.startsWith(u8, rewrite.old, "typedef unsigned int"));
    try std.testing.expect(std.mem.endsWith(u8, rewrite.old, "ULONG;"));
    try std.testing.expect(std.mem.startsWith(u8, rewrite.new, "typedef unsigned long"));
    try std.testing.expect(std.mem.endsWith(u8, rewrite.new, "ULONG;"));
    try std.testing.expect(!std.mem.eql(u8, rewrite.old, rewrite.new));
}

test "the vendored and first-party source sets are named" {
    try std.testing.expectEqual(@as(usize, 4), host.vendored_dirs.len);
    try std.testing.expectEqual(@as(usize, 2), host.first_party_sources.len);
    try std.testing.expect(std.mem.endsWith(u8, host.first_party_sources[0], "ra8_ipif.c"));
    try std.testing.expect(std.mem.endsWith(u8, host.first_party_sources[1], "netx_ipif_harness.c"));
    // The generated tx_port.h copy goes first on the include path.
    try std.testing.expect(!std.mem.startsWith(u8, host.include_paths[0], "pkg:netxduo"));
    try std.testing.expectEqual(@as(usize, 8), host.include_paths.len);
}
