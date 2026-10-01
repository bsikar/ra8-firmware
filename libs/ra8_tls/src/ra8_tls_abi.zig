//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C membrane for `ra8_tls.h`: the ten exported entry points, the raw
//! pointers they receive, and the log lines the C emitted. Everything below
//! this file works in slices and optionals.
//!
//! External symbols: the four `ra8_log_emit_*` sinks. The log conditions are
//! re-derived from what the facade returns, so the facade itself stays free
//! of C externs and runs in the host suite.

const cfg = @import("internal/cfg.zig");
const facade = @import("internal/facade.zig");
const mss = @import("internal/mss.zig");
const vocab = @import("internal/vocab.zig");

const Err = vocab.Err;
const Session = facade.Slot;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_warn(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void;

const tag: [*:0]const u8 = "ra8_tls";

/// Build the slice the facade wants from a caller's pointer and length.
/// Null with a non-zero length is the one shape that must be rejected, and
/// it arrives as a null optional.
fn readable(buf: ?[*]const u8, len: usize) ?[]const u8 {
    const raw = buf orelse return if (len == 0) &.{} else null;
    return raw[0..len];
}

/// A real zero-length object, so a null pointer with a zero length can still
/// become a (mutable) empty slice rather than a rejected call.
var no_bytes: [0]u8 = [_]u8{};

fn writable(buf: ?[*]u8, len: usize) ?[]u8 {
    const raw = buf orelse return if (len == 0) no_bytes[0..] else null;
    return raw[0..len];
}

export fn ra8_tls_global_init() u16 {
    const status = facade.globalInit();
    switch (status) {
        Err.exists => ra8_log_emit_warn(tag, "global_init called twice"),
        Err.hw_error => ra8_log_emit_error(tag, "psa_crypto_init failed"),
        Err.ok => ra8_log_emit_info(tag, "global_init ok"),
        else => {},
    }
    return status;
}

export fn ra8_tls_global_deinit() u16 {
    return facade.globalDeinit();
}

export fn ra8_tls_session_open(out_session: ?*?*Session, config: ?*const cfg.Session) u16 {
    const out = out_session orelse return Err.invalid_arg;
    out.* = null;
    const status = facade.sessionOpen(config, out);
    if (status == Err.no_mem) ra8_log_emit_warn(tag, "session pool exhausted");
    return status;
}

export fn ra8_tls_session_close(session: ?*Session) u16 {
    return facade.sessionClose(session);
}

export fn ra8_tls_handshake(session: ?*Session) u16 {
    const status = facade.handshake(session);
    if (status == Err.comm_error) ra8_log_emit_error(tag, "handshake failed");
    return status;
}

export fn ra8_tls_send(session: ?*Session, buf: ?[*]const u8, len: usize, out_sent: ?*usize) u16 {
    const sent = out_sent orelse return Err.invalid_arg;
    sent.* = 0;
    return facade.send(session, readable(buf, len), sent);
}

export fn ra8_tls_recv(session: ?*Session, buf: ?[*]u8, len: usize, out_received: ?*usize) u16 {
    const received = out_received orelse return Err.invalid_arg;
    received.* = 0;
    return facade.recv(session, writable(buf, len), received);
}

export fn ra8_tls_get_cipher_suite(
    session: ?*Session,
    out_id: ?*u16,
    out_name: ?[*]u8,
    name_cap: usize,
) u16 {
    const id = out_id orelse return Err.invalid_arg;
    const name = out_name orelse return Err.invalid_arg;
    if (name_cap == 0) return Err.invalid_arg;
    id.* = 0;
    name[0] = 0;
    return facade.cipherSuite(session, id, name[0..name_cap]);
}

export fn ra8_tls_get_verify_result(session: ?*Session, out_flags: ?*u32) u16 {
    const flags = out_flags orelse return Err.invalid_arg;
    flags.* = 0;
    return facade.verifyResult(session, flags);
}

export fn ra8_tls_mss_clamp(mtu: u16, out_mss: ?*u16) u16 {
    const out = out_mss orelse return Err.invalid_arg;
    out.* = 0;
    out.* = mss.clamp(mtu) orelse return Err.invalid_arg;
    return Err.ok;
}
