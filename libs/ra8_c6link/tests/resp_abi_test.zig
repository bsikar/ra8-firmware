//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_c6link_resp`: the fault slot it leaves behind and the RA8 error each
//! ESP wire status becomes.

const std = @import("std");
const resp_abi = @import("resp_abi");

const c = resp_abi.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const no_mem: u16 = 0x102;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const invalid_size: u16 = 0x105;
    pub const not_found: u16 = 0x106;
    pub const not_supported: u16 = 0x107;
    pub const timeout: u16 = 0x108;
    pub const access_denied: u16 = 0x112;
    pub const protocol_error: u16 = 0x406;
    pub const checksum_mismatch: u16 = 0x502;
    pub const null_ptr: u16 = 0x504;
};

const Mapping = struct { esp: i32, ra8: u16 };

const mappings = [_]Mapping{
    .{ .esp = 0x101, .ra8 = Code.no_mem },
    .{ .esp = 0x102, .ra8 = Code.invalid_arg },
    .{ .esp = 0x103, .ra8 = Code.invalid_state },
    .{ .esp = 0x104, .ra8 = Code.invalid_size },
    .{ .esp = 0x105, .ra8 = Code.not_found },
    .{ .esp = 0x106, .ra8 = Code.not_supported },
    .{ .esp = 0x107, .ra8 = Code.timeout },
    .{ .esp = 0x108, .ra8 = Code.protocol_error },
    .{ .esp = 0x109, .ra8 = Code.checksum_mismatch },
    .{ .esp = 0x10D, .ra8 = Code.access_denied },
    .{ .esp = -1, .ra8 = Code.protocol_error },
    .{ .esp = 0x3009, .ra8 = Code.protocol_error },
};

test "each nonzero status maps to its RA8 error and names the request" {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    for (mappings) |m| {
        link.fault = .{ .rpc_id = 0, .resp = 0 };
        try std.testing.expectEqual(m.ra8, resp_abi.priv_c6link_resp(&link, 300, m.esp));
        try std.testing.expectEqual(@as(u32, 300), link.fault.rpc_id);
        try std.testing.expectEqual(m.esp, link.fault.resp);
    }
}

test "a zero verdict clears an earlier fault" {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.fault = .{ .rpc_id = 77, .resp = 0x103 };
    try std.testing.expectEqual(Code.ok, resp_abi.priv_c6link_resp(&link, 300, 0));
    try std.testing.expectEqual(@as(u32, 0), link.fault.rpc_id);
    try std.testing.expectEqual(@as(i32, 0), link.fault.resp);
}

test "a null link is null_ptr" {
    try std.testing.expectEqual(Code.null_ptr, resp_abi.priv_c6link_resp(null, 300, 0x101));
}
