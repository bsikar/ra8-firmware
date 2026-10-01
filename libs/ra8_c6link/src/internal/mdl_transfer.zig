//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Bounded transactional coordinator for C6 media byte streams.
//!
//! Composes the three seams the caller injects: the remote media RPC, a
//! transactional local destination, and an independent running digest. It owns
//! no static state and allocates nothing.
//!
//! Status codes are relayed, not re-made. Every injected seam hands back an
//! `ra8_err_t`, and a failure from one of them has to reach the caller exactly
//! as it arrived, so this layer speaks in `u16` codes rather than a Zig error
//! set: mapping an arbitrary relayed code into a closed set and back could
//! only lose it.

const std = @import("std");

const types = @import("mdl_types.zig");

/// The `ra8_err_t` values this layer produces on its own.
pub const Err = struct {
    pub const ok: u16 = 0;
    pub const fail: u16 = 0x101;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const invalid_size: u16 = 0x105;
    pub const timeout: u16 = 0x108;
    pub const cancelled: u16 = 0x10E;
    pub const checksum_mismatch: u16 = 0x502;
    pub const null_ptr: u16 = 0x504;
};

/// The media RPC, still C in `ra8_c6link_mdl.c`. The link handle is opaque
/// here: the coordinator hands it straight back and never reads a field.
extern fn ra8_c6link_mdl_start_request(
    link: ?*anyopaque,
    request: *const types.Request,
    session: *types.Session,
) callconv(.c) u16;
extern fn ra8_c6link_mdl_next(
    link: ?*anyopaque,
    session: *types.Session,
    max_bytes: u16,
    chunk: *types.Chunk,
) callconv(.c) u16;
extern fn ra8_c6link_mdl_cancel(link: ?*anyopaque, session: *types.Session) callconv(.c) u16;

/// State that must be unwound together once storage has begun.
pub const State = struct {
    /// Active media RPC link, opaque to this layer.
    link: ?*anyopaque = null,
    /// Injected storage and hash seams.
    config: *const types.Config,
    /// Correlated remote session.
    session: types.Session = .{},
    /// Whether local temporary storage still needs an abort.
    storage_active: bool = false,
};

/// Reject every unusable injected mechanism before any temporary state exists.
///
/// Success guarantees each later indirect call target is non-null, so the rest
/// of the coordinator calls them without re-checking.
fn validate(config: *const types.Config) u16 {
    const storage = config.storage;
    const digest = config.sha256;
    if (storage.begin == null or storage.write == null or storage.commit == null or
        storage.abort == null or storage.ctx == null or digest.init == null or
        digest.update == null or digest.final == null or digest.ctx == null)
    {
        return Err.null_ptr;
    }
    if (config.format > types.Format.rabook) return Err.invalid_arg;
    if (config.format != types.Format.loose and storage.validate == null) return Err.null_ptr;

    const budget = @as(u64, config.chunk_bytes) * config.max_chunks;
    if (config.chunk_bytes == 0 or config.chunk_bytes > types.Limit.chunk_data_max or
        config.max_chunks == 0 or budget > types.Limit.transfer_bytes_max)
    {
        return Err.invalid_size;
    }
    return Err.ok;
}

/// Cancel the remote job when its session is live, and always abort local
/// temporary state.
///
/// Cleanup failures surface only when no earlier cause exists: cause first,
/// then cancel, then abort.
pub fn unwind(state: *State, cause: u16) u16 {
    var cancel_result: u16 = Err.ok;
    var abort_result: u16 = Err.ok;
    if (state.session.active) {
        cancel_result = ra8_c6link_mdl_cancel(state.link, &state.session);
    }
    if (state.storage_active) {
        abort_result = state.config.storage.abort.?(state.config.storage.ctx);
        state.storage_active = false;
    }
    if (cause != Err.ok) return cause;
    return if (cancel_result != Err.ok) cancel_result else abort_result;
}

/// Persist and hash one non-terminal ordered chunk.
///
/// A short write is refused: it would leave the digest describing bytes that
/// are not durable in the temporary object.
fn store(config: *const types.Config, chunk: *const types.Chunk, bytes_stored: *u64) u16 {
    if (bytes_stored.* > std.math.maxInt(u64) - @as(u64, chunk.data_len)) return Err.invalid_size;

    var written: u16 = 0;
    const err = config.storage.write.?(
        config.storage.ctx,
        &chunk.data,
        chunk.data_len,
        &written,
    );
    if (err != Err.ok) return err;
    if (written != chunk.data_len) return Err.invalid_size;

    const hashed = config.sha256.update.?(config.sha256.ctx, &chunk.data, chunk.data_len);
    if (hashed == Err.ok) bytes_stored.* += written;
    return hashed;
}

/// Verify terminal metadata, validate the artifact, and commit the object.
///
/// The digest is finalised only after the byte counts agree, and an optional
/// artifact validator sees the complete private object before publication.
pub fn commit(
    state: *State,
    chunk: *const types.Chunk,
    bytes_stored: u64,
    chunks_received: u32,
    result: *types.Result,
) u16 {
    if (!chunk.has_sha256 or chunk.total_bytes != bytes_stored) return Err.invalid_size;

    var digest: [types.Limit.sha256_bytes]u8 = @splat(0);
    const finalised = state.config.sha256.final.?(state.config.sha256.ctx, &digest);
    if (finalised != Err.ok) return finalised;
    if (!std.mem.eql(u8, &digest, &chunk.sha256)) return Err.checksum_mismatch;

    if (state.config.storage.validate) |check| {
        const checked = check(state.config.storage.ctx, bytes_stored, &digest);
        if (checked != Err.ok) return checked;
    }

    const err = state.config.storage.commit.?(state.config.storage.ctx);
    if (err == Err.ok) {
        result.* = .{
            .bytes_stored = bytes_stored,
            .chunks_received = chunks_received,
            .format = state.config.format,
            .sha256 = digest,
            .response = chunk.response,
        };
        state.storage_active = false;
    }
    return err;
}

/// Validate the configuration, then open local storage and the remote job.
///
/// `state.storage_active` ends true exactly when `storage.begin` succeeded,
/// which is what tells the caller whether cleanup is owed.
fn begin(
    link: ?*anyopaque,
    url: [*:0]const u8,
    destination: [*:0]const u8,
    config: *const types.Config,
    result: *types.Result,
    state: *State,
) u16 {
    const validation = validate(config);
    if (validation != Err.ok) return validation;

    result.* = .{};
    state.link = link;

    const opened = config.storage.begin.?(config.storage.ctx, destination);
    if (opened != Err.ok) return opened;
    state.storage_active = true;

    const started = config.sha256.init.?(config.sha256.ctx);
    if (started != Err.ok) return started;

    const request = types.Request{ .url = url, .format = config.format, .http = config.http };
    return ra8_c6link_mdl_start_request(link, &request, &state.session);
}

/// Run one complete transfer: start, pull until a terminal response, commit.
pub fn transfer(
    link: ?*anyopaque,
    url: [*:0]const u8,
    destination: [*:0]const u8,
    config: *const types.Config,
    result: *types.Result,
) u16 {
    var state = State{ .config = config };
    var err = begin(link, url, destination, config, result, &state);
    if (!state.storage_active) return err;

    var bytes_stored: u64 = 0;
    var pull: u32 = 0;
    while (pull < config.max_chunks and err == Err.ok) : (pull += 1) {
        if (config.cancel_requested) |asked| {
            if (asked(config.cancel_ctx)) {
                err = Err.cancelled;
                break;
            }
        }

        var chunk = types.Chunk{};
        err = ra8_c6link_mdl_next(link, &state.session, config.chunk_bytes, &chunk);
        if (err != Err.ok) continue;

        switch (chunk.state) {
            types.State.downloading => err = store(config, &chunk, &bytes_stored),
            types.State.complete => {
                err = commit(&state, &chunk, bytes_stored, pull + 1, result);
                return if (err == Err.ok) Err.ok else unwind(&state, err);
            },
            // A chunk that reaches here with err == ok is necessarily
            // CANCELLED: DOWNLOADING and COMPLETE are taken above, and the
            // RPC's own validator rejects ACCEPTED and every unassigned state
            // value and requires FAILED to carry a nonzero status, which it
            // relays verbatim. The arm is kept for the state machine to be
            // readable, not because another value can arrive.
            else => err = Err.cancelled,
        }
    }

    // Reaching here with err == ok means the chunk budget was exhausted, and
    // the session can only still be active in that case: every terminal
    // response either returns from inside the loop or sets a cause.
    if (err == Err.ok and state.session.active) err = Err.timeout;
    return unwind(&state, err);
}
