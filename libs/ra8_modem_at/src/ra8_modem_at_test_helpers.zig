//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C-test-only exports for the private MC/DC helpers. The firmware archive
//! exposes only the public C ABI.

const abi = @import("abi");

pub export fn priv_modem_str_len(s: [*:0]const u8) callconv(.c) u16 {
    return abi.priv_modem_str_len(s);
}

pub export fn priv_modem_starts_with(hay: [*:0]const u8, needle: [*:0]const u8) callconv(.c) u8 {
    return abi.priv_modem_starts_with(hay, needle);
}

pub export fn priv_modem_str_eq(a: [*:0]const u8, b: [*:0]const u8) callconv(.c) u8 {
    return abi.priv_modem_str_eq(a, b);
}

pub export fn priv_modem_classify(
    line: [*:0]const u8,
    cmd_echo: ?[*:0]const u8,
    expected_response: ?[*:0]const u8,
) callconv(.c) u8 {
    return abi.priv_modem_classify(line, cmd_echo, expected_response);
}

pub export fn priv_modem_capture_line(
    line: [*:0]const u8,
    capture: ?[*]u8,
    capture_len: usize,
    used: ?*usize,
) callconv(.c) void {
    abi.priv_modem_capture_line(line, capture, capture_len, used);
}

pub export fn priv_modem_reset_line_should_clear(
    line_buf: ?*const anyopaque,
    line_buf_len: u16,
) callconv(.c) u8 {
    return abi.priv_modem_reset_line_should_clear(line_buf, line_buf_len);
}

pub export fn priv_modem_payload_prefix_matches(
    line: [*:0]const u8,
    expected_response: ?[*:0]const u8,
) callconv(.c) u8 {
    return abi.priv_modem_payload_prefix_matches(line, expected_response);
}

pub export fn priv_modem_capture_should_clear(
    capture: ?*const anyopaque,
    capture_len: usize,
) callconv(.c) u8 {
    return abi.priv_modem_capture_should_clear(capture, capture_len);
}
