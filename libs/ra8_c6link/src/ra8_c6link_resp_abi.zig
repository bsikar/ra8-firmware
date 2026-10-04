//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for `priv_c6link_resp`: record the co-processor's verdict on one
//! request in the link's fault slot and translate it into the RA8 error
//! domain. Every response extractor ends here. The rest of the RPC layer
//! stays in C for now (RA8FW-644).

const Err = @import("abi_err.zig");
const header = @import("c6link_rpc_c.zig");

pub const c = header.c;

/// The ESP-IDF general error values the co-processor puts on the wire. These
/// are ESP-IDF's published numbers, so they never move under us.
const EspErr = struct {
    const no_mem: i32 = 0x101;
    const invalid_arg: i32 = 0x102;
    const invalid_state: i32 = 0x103;
    const invalid_size: i32 = 0x104;
    const not_found: i32 = 0x105;
    const not_supported: i32 = 0x106;
    const timeout: i32 = 0x107;
    const invalid_response: i32 = 0x108;
    const invalid_crc: i32 = 0x109;
    const not_allowed: i32 = 0x10D;
};

/// The closest RA8 error for a nonzero ESP status. Actionable general
/// failures keep their meaning; `invalid_response`, component-specific codes
/// and anything unknown become `protocol_error` (the raw value stays in the
/// fault slot for anyone who needs it).
fn remoteError(response: i32) c.ra8_err_t {
    return switch (response) {
        EspErr.no_mem => Err.no_mem,
        EspErr.invalid_arg => Err.invalid_arg,
        EspErr.invalid_state => Err.invalid_state,
        EspErr.invalid_size => Err.invalid_size,
        EspErr.not_found => Err.not_found,
        EspErr.not_supported => Err.not_supported,
        EspErr.timeout => Err.timeout,
        EspErr.invalid_crc => Err.checksum_mismatch,
        EspErr.not_allowed => Err.access_denied,
        else => Err.protocol_error,
    };
}

/// `priv_c6link_resp`: a null link is `null_ptr`. A nonzero `resp` names
/// `rpc_id` and `resp` in the fault slot and returns the mapped error; zero
/// clears the slot and returns `ok`.
pub export fn priv_c6link_resp(link: ?*c.ra8_c6link_t, rpc_id: u32, resp: i32) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    if (resp != 0) {
        handle.fault.rpc_id = rpc_id;
        handle.fault.resp = resp;
        return remoteError(resp);
    }
    handle.fault.rpc_id = 0;
    handle.fault.resp = 0;
    return Err.ok;
}
