//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The on-target backend: the only file that speaks Mbed TLS's
//! negative-errno dialect. The BIO pair below is the whole of the vendor
//! seam, so an application authors a transport against `ra8_tls.h` alone.

const cfg = @import("cfg.zig");
const mbedtls = @import("mbedtls_c.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const c = mbedtls.c;

/// What a live session carries beyond its configuration. `session` points at
/// the pool slot's own copy of the caller config, which is what lets a BIO
/// shim reach the transport with only this state as its context.
pub const SlotState = extern struct {
    ssl: c.mbedtls_ssl_context,
    config: c.mbedtls_ssl_config,
    ca: c.mbedtls_x509_crt,
    session: ?*const cfg.Session,
};

pub fn globalInit() u16 {
    // Mbed TLS 4.x draws randomness from PSA crypto rather than a
    // facade-owned CTR_DRBG. Entropy arrives lazily through the
    // application's `mbedtls_psa_external_get_random` hook.
    if (c.psa_crypto_init() != mbedtls.psa_success) return Err.hw_error;
    return Err.ok;
}

/// Map a house transport result onto the signed count Mbed TLS expects.
fn bioResult(status: u16, moved: usize, blocked: c_int) c_int {
    if (status == Err.would_block) return blocked;
    if (status != Err.ok) return mbedtls.internal_error;
    if (moved > @as(usize, @intCast(@as(c_int, @bitCast(@as(u32, 0x7fff_ffff)))))) {
        return mbedtls.internal_error;
    }
    return @intCast(moved);
}

fn bioSend(ctx: ?*anyopaque, buf: [*c]const u8, len: usize) callconv(.c) c_int {
    const state: *SlotState = @ptrCast(@alignCast(ctx.?));
    const transport = state.session.?.transport;
    var sent: usize = 0;
    const status = transport.send.?(transport.ctx, buf, len, &sent);
    return bioResult(status, sent, mbedtls.want_write);
}

fn bioRecv(ctx: ?*anyopaque, buf: [*c]u8, len: usize) callconv(.c) c_int {
    const state: *SlotState = @ptrCast(@alignCast(ctx.?));
    const transport = state.session.?.transport;
    var received: usize = 0;
    const status = transport.recv.?(transport.ctx, buf, len, &received);
    // End of stream is the house spelling, ok with zero bytes, and becomes
    // the plain 0 Mbed TLS reads as a clean close.
    return bioResult(status, received, mbedtls.want_read);
}

fn authMode(mode: vocab.VerifyMode) c_int {
    return switch (mode) {
        .none => mbedtls.verify_none,
        .optional => mbedtls.verify_optional,
        else => mbedtls.verify_required,
    };
}

/// Apply the verify policy and, when the caller supplied one, the trust
/// anchor. A PEM that will not parse is non-fatal: the handshake still runs
/// and a required verify simply fails, reported through `verifyResult`.
fn applyVerify(state: *SlotState, session: *const cfg.Session) void {
    c.mbedtls_ssl_conf_authmode(&state.config, authMode(session.verify_mode));
    c.mbedtls_x509_crt_init(&state.ca);
    const pem = session.caPem() orelse return;
    if (c.mbedtls_x509_crt_parse(&state.ca, pem.ptr, pem.len) != 0) return;
    c.mbedtls_ssl_conf_ca_chain(&state.config, &state.ca, null);
}

fn teardown(state: *SlotState) void {
    c.mbedtls_ssl_free(&state.ssl);
    c.mbedtls_ssl_config_free(&state.config);
    c.mbedtls_x509_crt_free(&state.ca);
}

pub fn sessionSetup(state: *SlotState, session: *const cfg.Session) u16 {
    state.session = session;
    c.mbedtls_ssl_init(&state.ssl);
    c.mbedtls_ssl_config_init(&state.config);

    const defaults = c.mbedtls_ssl_config_defaults(
        &state.config,
        mbedtls.is_client,
        mbedtls.transport_stream,
        mbedtls.preset_default,
    );
    if (defaults != 0) {
        c.mbedtls_ssl_free(&state.ssl);
        c.mbedtls_ssl_config_free(&state.config);
        return Err.hw_init_failed;
    }

    applyVerify(state, session);

    if (c.mbedtls_ssl_setup(&state.ssl, &state.config) != 0) {
        teardown(state);
        return Err.hw_init_failed;
    }

    if (session.server_name) |name| _ = c.mbedtls_ssl_set_hostname(&state.ssl, name);
    c.mbedtls_ssl_set_bio(&state.ssl, state, bioSend, bioRecv, null);
    return Err.ok;
}

pub fn sessionFree(state: *SlotState) void {
    teardown(state);
}

/// Shared tail of handshake / read / write: the two would-block codes are
/// the house `would_block`, anything else negative is a comm error.
fn blockedOr(rc: c_int, otherwise: u16) u16 {
    if (rc == mbedtls.want_read or rc == mbedtls.want_write) return Err.would_block;
    return otherwise;
}

pub fn handshake(state: *SlotState, session: *const cfg.Session) u16 {
    _ = session;
    const rc = c.mbedtls_ssl_handshake(&state.ssl);
    if (rc == 0) return Err.ok;
    return blockedOr(rc, Err.comm_error);
}

pub fn send(state: *SlotState, session: *const cfg.Session, buf: []const u8, out_sent: *usize) u16 {
    _ = session;
    const rc = c.mbedtls_ssl_write(&state.ssl, buf.ptr, buf.len);
    if (rc >= 0) {
        out_sent.* = @intCast(rc);
        return Err.ok;
    }
    return blockedOr(rc, Err.comm_error);
}

pub fn recv(state: *SlotState, session: *const cfg.Session, buf: []u8, out_received: *usize) u16 {
    _ = session;
    const rc = c.mbedtls_ssl_read(&state.ssl, buf.ptr, buf.len);
    if (rc >= 0) {
        out_received.* = @intCast(rc);
        return Err.ok;
    }
    if (rc == mbedtls.peer_close_notify) return Err.ok;
    return blockedOr(rc, Err.comm_error);
}

pub fn cipherSuite(state: *const SlotState, out_id: *u16, out_name: *[]const u8) void {
    const mutable: *SlotState = @constCast(state);
    const name = c.mbedtls_ssl_get_ciphersuite(&mutable.ssl) orelse return;
    out_id.* = @truncate(@as(u32, @bitCast(c.mbedtls_ssl_get_ciphersuite_id_from_ssl(&mutable.ssl))));
    out_name.* = @import("std").mem.span(name);
}

pub fn verifyResult(state: *const SlotState) u32 {
    const mutable: *SlotState = @constCast(state);
    return c.mbedtls_ssl_get_verify_result(&mutable.ssl);
}
