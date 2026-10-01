//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `RA8_OFF_TARGET` backend: no TLS at all, just enough to drive the
//! transport seam end to end. A handshake is one byte out and one byte back,
//! and payloads pass straight through, so the host suite exercises the BIO
//! contract with Mbed TLS unlinked.

const cfg = @import("cfg.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;

/// The vocabulary this backend speaks, re-exported so a caller (and the host
/// suite) needs only this file.
pub const Session = cfg.Session;
pub const Status = vocab.Err;

/// TLS record content type: Handshake (22). The one byte the fake sends.
pub const content_handshake: u8 = 0x16;

/// Cipher-suite name this backend always reports.
pub const cipher_name = "off-target-loopback";

/// What a live session carries beyond its configuration.
pub const SlotState = extern struct {
    handshake_done: bool = false,
};

pub fn globalInit() u16 {
    return Err.ok;
}

pub fn sessionSetup(state: *SlotState, session: *const cfg.Session) u16 {
    _ = session;
    state.handshake_done = false;
    return Err.ok;
}

pub fn sessionFree(state: *SlotState) void {
    state.handshake_done = false;
}

/// Map a transport result onto what the handshake reports: a would-block
/// stays itself, every other failure is a comm error.
fn transportFault(status: u16) u16 {
    return if (status == Err.would_block) Err.would_block else Err.comm_error;
}

pub fn handshake(state: *SlotState, session: *const cfg.Session) u16 {
    const transport = session.transport;

    var out_byte: u8 = content_handshake;
    var sent: usize = 0;
    const send_rc = transport.send.?(transport.ctx, @ptrCast(&out_byte), 1, &sent);
    if (send_rc != Err.ok) return transportFault(send_rc);

    var in_byte: u8 = 0;
    var received: usize = 0;
    const recv_rc = transport.recv.?(transport.ctx, @ptrCast(&in_byte), 1, &received);
    if (recv_rc != Err.ok) return transportFault(recv_rc);

    state.handshake_done = true;
    return Err.ok;
}

pub fn send(state: *SlotState, session: *const cfg.Session, buf: []const u8, out_sent: *usize) u16 {
    _ = state;
    const transport = session.transport;
    return transport.send.?(transport.ctx, buf.ptr, buf.len, out_sent);
}

pub fn recv(state: *SlotState, session: *const cfg.Session, buf: []u8, out_received: *usize) u16 {
    _ = state;
    const transport = session.transport;
    return transport.recv.?(transport.ctx, buf.ptr, buf.len, out_received);
}

/// The fake has no negotiated suite, so the id stays zero and only the
/// deterministic name is reported.
pub fn cipherSuite(state: *const SlotState, out_id: *u16, out_name: *[]const u8) void {
    _ = state;
    out_id.* = 0;
    out_name.* = cipher_name;
}

pub fn verifyResult(state: *const SlotState) u32 {
    _ = state;
    return 0;
}
