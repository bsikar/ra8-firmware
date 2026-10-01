//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The facade proper: module state, the order the checks run in, and the
//! hand-off to whichever backend the build selected. Everything here works
//! in slices; `ra8_tls_abi.zig` above converts the caller's raw pointers,
//! and the backend below owns the protocol.

const build_config = @import("build_config");
const cfg = @import("cfg.zig");
const cstr = @import("cstr.zig");
const pool = @import("pool.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Limits = vocab.Limits;

/// The C chose its stack with `#ifdef RA8_OFF_TARGET`; this is the same
/// switch. Only the taken arm is analysed, so a host build never reaches the
/// `@cImport` of the vendored Mbed TLS headers.
const backend = if (build_config.off_target)
    @import("backend_fake.zig")
else
    @import("backend_mbedtls.zig");

/// The caller-facing records and status codes, re-exported so the membrane
/// above and the host suite need only this file.
pub const Session = cfg.Session;
pub const Status = vocab.Err;

/// One pool slot, backing exactly one `ra8_tls_session_t`.
pub const Slot = struct {
    cfg: cfg.Session,
    state: backend.SlotState,
};

const Sessions = pool.Pool(Slot, Limits.max_sessions);

var sessions: Sessions = .{};
var initialized: bool = false;

fn releaseOne(_: void, slot: *Slot) void {
    backend.sessionFree(&slot.state);
}

fn live(handle: ?*Slot) ?*Slot {
    if (!sessions.owns(handle)) return null;
    return handle;
}

pub fn globalInit() u16 {
    if (initialized) return Err.exists;
    sessions.reset({}, releaseOne);
    const status = backend.globalInit();
    if (status != Err.ok) return status;
    initialized = true;
    return Err.ok;
}

pub fn globalDeinit() u16 {
    if (!initialized) return Err.not_initialized;
    sessions.reset({}, releaseOne);
    initialized = false;
    return Err.ok;
}

pub fn sessionOpen(config: ?*const cfg.Session, out: *?*Slot) u16 {
    if (!initialized) return Err.not_initialized;
    const want = config orelse return Err.invalid_arg;
    if (want.transport.send == null or want.transport.recv == null) return Err.invalid_arg;

    const slot = sessions.acquire() orelse return Err.no_mem;
    slot.cfg = want.*;
    const status = backend.sessionSetup(&slot.state, &slot.cfg);
    if (status != Err.ok) {
        sessions.release(slot);
        return status;
    }
    out.* = slot;
    return Err.ok;
}

pub fn sessionClose(handle: ?*Slot) u16 {
    if (!initialized) return Err.not_initialized;
    const slot = live(handle) orelse return Err.invalid_arg;
    backend.sessionFree(&slot.state);
    sessions.release(slot);
    return Err.ok;
}

pub fn handshake(handle: ?*Slot) u16 {
    if (!initialized) return Err.not_initialized;
    const slot = live(handle) orelse return Err.invalid_arg;
    return backend.handshake(&slot.state, &slot.cfg);
}

/// `buf` is null when the caller passed a null pointer with a non-zero
/// length, which is the one shape the C rejects outright.
pub fn send(handle: ?*Slot, buf: ?[]const u8, out_sent: *usize) u16 {
    if (!initialized) return Err.not_initialized;
    const slot = live(handle) orelse return Err.invalid_arg;
    const bytes = buf orelse return Err.invalid_arg;
    if (bytes.len == 0) return Err.ok;
    return backend.send(&slot.state, &slot.cfg, bytes, out_sent);
}

pub fn recv(handle: ?*Slot, buf: ?[]u8, out_received: *usize) u16 {
    if (!initialized) return Err.not_initialized;
    const slot = live(handle) orelse return Err.invalid_arg;
    const bytes = buf orelse return Err.invalid_arg;
    if (bytes.len == 0) return Err.ok;
    return backend.recv(&slot.state, &slot.cfg, bytes, out_received);
}

pub fn cipherSuite(handle: ?*Slot, out_id: *u16, out_name: []u8) u16 {
    if (!initialized) return Err.not_initialized;
    const slot = live(handle) orelse return Err.invalid_arg;
    var name: []const u8 = &.{};
    backend.cipherSuite(&slot.state, out_id, &name);
    if (name.len != 0) cstr.write(out_name, name);
    return Err.ok;
}

pub fn verifyResult(handle: ?*Slot, out_flags: *u32) u16 {
    if (!initialized) return Err.not_initialized;
    const slot = live(handle) orelse return Err.invalid_arg;
    out_flags.* = backend.verifyResult(&slot.state);
    return Err.ok;
}

/// Test-only: drop all state so each host case starts from a cold module.
pub fn resetForTest() void {
    sessions = .{};
    initialized = false;
}
