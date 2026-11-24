//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the `ra8_ota` orchestration state machine and the
//! signature-verification cluster: what used to be `src/ra8_ota.c` and
//! `src/ra8_ota_verify.c`.
//!
//! This file owns the four module statics `src/ra8_ota_internal.h` declares
//! `extern` (`g_ra8_ota_cfg`, `g_ra8_ota_state`, `g_ra8_ota_initialized`,
//! `g_ra8_ota_buf`) plus `priv_ota_set_state`, so three C suites keep poking
//! exactly the symbols and layouts they poked before. The parsing membrane
//! rides in the same archive and is reached through `parse`; every pure
//! predicate and every piece of arithmetic sits in `internal/root.zig`.
//!
//! Guard order, log lines and error codes are the C's, byte for byte.
//!
//! Pipeline contract alignment (`docs/design/unified_update_pipeline.md`):
//!   - `ra8_ota_check_for_update`          -> PREFLIGHT (HTTPS/RAM JSON manifest pull)
//!   - `ra8_ota_download_to_inactive_bank` -> STAGE     (32B-aligned chunked flash writes)
//!   - `ra8_ota_verify_signature`          -> VERIFY    (rehash + bound metadata ECDSA; failure skips COMMIT)
//!   - `ra8_ota_commit_and_reboot`         -> COMMIT    (bank swap / `ra8_nsc_ota_commit` NSC veneer)
//! Divergences from DFU flow: executes in NS world using pull-based JSON manifest +
//! immediate verify-before-swap (`trial_budget = 0`) rather than S-world copy-to-run
//! slots (`"RA8D"` / `ROT1`) with WDT0 trial countdown.

const std = @import("std");
const Err = @import("err").Err;
const pl = @import("pipeline");
const sess = @import("session");
const bn = @import("bundle");
const impl = @import("internal/root.zig");
const ota_pl = @import("ota_pipeline.zig");

/// The parsing/validation membrane. Imported (not re-declared) so both
/// clusters land in one archive and share one copy of the struct types.
pub const parse = @import("ra8_ota_parse_abi.zig");

/// Re-exported so the ABI tests share these exact types with the membrane.
pub const internal = impl;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// The weak hook below, reached through the symbol table so a strong
/// definition in the firmware (or in a test suite) overrides it at link time.
extern fn ra8_ota_system_reset_hook() void;

/// The C duplicates this immutable literal in every ra8_ota TU.
const tag: [*:0]const u8 = "ra8_ota";

/// `RA8_CHECK_NULL_PTR(ptr, s_tag, message)`: log then answer null_ptr.
fn refuseNull(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return impl.err.null_ptr;
}

// =============================================================================
// Shared mutable module state (ABI: declared extern in ra8_ota_internal.h)
// =============================================================================

/// `g_ra8_ota_state`: single-byte cooperative state machine value.
pub export var g_ra8_ota_state: u8 = impl.state.idle;

/// `g_ra8_ota_cfg`: the configuration captured by `ra8_ota_init`.
pub export var g_ra8_ota_cfg: impl.Cfg = std.mem.zeroes(impl.Cfg);

/// `g_ra8_ota_initialized`: true once `ra8_ota_init` has succeeded.
pub export var g_ra8_ota_initialized: bool = false;

/// `g_ra8_ota_buf`: the streaming chunk buffer shared by the manifest fetch,
/// the download pass and the re-hash pass.
pub export var g_ra8_ota_buf: [@as(usize, impl.chunk_bytes)]u8 = @splat(0);

// -----------------------------------------------------------------------------
// TU-private statics: these were `static` in ra8_ota.c and stay unexported.
// -----------------------------------------------------------------------------

/// Cached decoded manifest from the most recent check.
var s_manifest: impl.Manifest = std.mem.zeroes(impl.Manifest);

/// Whether `s_manifest` holds a valid payload.
var s_manifest_valid: bool = false;

/// Bytes already programmed into the inactive bank.
var s_bytes_done: u32 = 0;

/// Last error observed by the state machine.
var s_last_err: u16 = impl.err.ok;

/// Shared `ra8_update` pipeline session and bundle-translation state.
var s_pipeline: ota_pl.State = .{};

/// Expose shared-pipeline session diagnostics to tests without changing public headers.
pub fn pipelineState() *const ota_pl.State {
    return &s_pipeline;
}

// =============================================================================
// State transitions
// =============================================================================

/// `priv_ota_set_state`: latch the state plus the last error, then fan the
/// snapshot out to the caller's progress callback when one is registered.
pub export fn priv_ota_set_state(new_state: u8, e: u16) callconv(.c) void {
    g_ra8_ota_state = new_state;
    s_last_err = e;
    if (g_ra8_ota_cfg.on_progress) |on_progress| {
        const snap = impl.Progress{
            .state = new_state,
            .bytes_done = s_bytes_done,
            .bytes_total = impl.progressTotal(s_manifest_valid, s_manifest.image_size_bytes),
            .last_err = e,
        };
        on_progress(&snap);
    }
}

// =============================================================================
// Networking helpers
// =============================================================================

/// `internal_drain`: accumulate up to `cap` bytes, stopping at EOF. The
/// iteration bound is the C's `cap + 1U`, so it wraps where the C's would.
fn drain(dst: [*]u8, cap: u32, out_n: *u32) u16 {
    var total: u32 = 0;
    var guard: u32 = 0;
    while (guard < cap +% 1) : (guard += 1) {
        if (impl.drainFilled(total, cap)) break;
        var got: u32 = 0;
        const e = g_ra8_ota_cfg.net.read.?(
            g_ra8_ota_cfg.net.ctx,
            dst + @as(usize, total),
            cap - total,
            &got,
        );
        if (e != impl.err.ok) return e;
        if (got == 0) break; // EOF
        total += got;
    }
    out_n.* = total;
    return impl.err.ok;
}

/// `internal_fetch_manifest_payload`: open the manifest URL, refuse an
/// over-long advertised body, drain into `g_ra8_ota_buf` and NUL-terminate it.
fn fetchManifestPayload(out_got: *u32) u16 {
    var content_len: u32 = 0;
    var e = g_ra8_ota_cfg.net.open.?(
        g_ra8_ota_cfg.net.ctx,
        @ptrCast(&g_ra8_ota_cfg.manifest_url),
        &content_len,
    );
    if (e != impl.err.ok) return e;
    if (impl.manifestPayloadTooLarge(content_len)) {
        _ = g_ra8_ota_cfg.net.close.?(g_ra8_ota_cfg.net.ctx);
        return impl.err.invalid_size;
    }
    var got: u32 = 0;
    e = drain(&g_ra8_ota_buf, impl.manifest_drain_cap, &got);
    _ = g_ra8_ota_cfg.net.close.?(g_ra8_ota_cfg.net.ctx);
    if (e != impl.err.ok) return e;
    g_ra8_ota_buf[got] = 0; // NUL terminate so the JSON scanners can scan it.
    out_got.* = got;
    return impl.err.ok;
}

// =============================================================================
// Public API: lifecycle
// =============================================================================

pub export fn ra8_ota_init(cfg_or_null: ?*const impl.Cfg) callconv(.c) u16 {
    if (g_ra8_ota_initialized) return impl.err.invalid_state;
    const e = parse.priv_ota_validate_cfg(cfg_or_null);
    if (e != impl.err.ok) return e;
    g_ra8_ota_cfg = cfg_or_null.?.*;
    const pl_err = s_pipeline.initSession(&g_ra8_ota_cfg);
    if (pl_err != impl.err.ok) {
        g_ra8_ota_cfg = std.mem.zeroes(impl.Cfg);
        return pl_err;
    }
    g_ra8_ota_state = impl.state.idle;
    s_manifest_valid = false;
    s_bytes_done = 0;
    s_last_err = impl.err.ok;
    g_ra8_ota_initialized = true;
    // run_as_thread is honoured by an external adapter; on host the caller
    // drives ra8_ota_run_step() directly.
    return impl.err.ok;
}

pub export fn ra8_ota_deinit() callconv(.c) u16 {
    g_ra8_ota_initialized = false;
    g_ra8_ota_state = impl.state.idle;
    s_manifest_valid = false;
    s_bytes_done = 0;
    s_last_err = impl.err.ok;
    s_pipeline.reset();
    g_ra8_ota_cfg = std.mem.zeroes(impl.Cfg);
    return impl.err.ok;
}

pub export fn ra8_ota_get_state() callconv(.c) u8 {
    return g_ra8_ota_state;
}

// =============================================================================
// Public API: manifest check
// =============================================================================

pub export fn ra8_ota_check_for_update(out_manifest_or_null: ?*impl.Manifest) callconv(.c) u16 {
    if (!g_ra8_ota_initialized) return impl.err.not_initialized;
    const out_manifest = out_manifest_or_null orelse return refuseNull("out_manifest");
    if (g_ra8_ota_state != impl.state.idle) return impl.err.invalid_state;
    priv_ota_set_state(impl.state.checking, impl.err.ok);

    var got: u32 = 0;
    var e = fetchManifestPayload(&got);
    if (e != impl.err.ok) {
        priv_ota_set_state(impl.state.failed, e);
        return e;
    }

    e = parse.priv_ota_manifest_decode(@ptrCast(&g_ra8_ota_buf), out_manifest);
    if (e != impl.err.ok) {
        priv_ota_set_state(impl.state.failed, e);
        return e;
    }
    s_manifest = out_manifest.*;
    s_manifest_valid = true;
    priv_ota_set_state(impl.state.idle, impl.err.ok);
    return impl.err.ok;
}

// =============================================================================
// Public API: download
// =============================================================================

/// `internal_download_chunk`: drain one chunk, stream-hash raw payloads, and
/// push bytes through `s_pipeline.session` stage storage.
fn downloadChunk(total: u32) u16 {
    const want = impl.chunkWant(total - s_bytes_done);
    var got: u32 = 0;
    var e = drain(&g_ra8_ota_buf, want, &got);
    if (e != impl.err.ok) return e;
    if (got == 0) return impl.err.hw_error;
    const bundle_start = (s_bytes_done == 0 and
        total >= bn.header_bytes and
        got >= 4 and
        std.mem.readInt(u32, g_ra8_ota_buf[0..4], .little) == bn.magic);
    if (!s_pipeline.bundle_mode and !bundle_start) {
        e = g_ra8_ota_cfg.crypto.sha256_update.?(g_ra8_ota_cfg.crypto.ctx, &g_ra8_ota_buf, got);
        if (e != impl.err.ok) return e;
    }
    e = sess.stageWrite(&s_pipeline.session, s_bytes_done, &g_ra8_ota_buf, got);
    if (e != impl.err.ok) return e;
    s_bytes_done += got;
    priv_ota_set_state(impl.state.downloading, impl.err.ok);
    return impl.err.ok;
}

/// `internal_prepare_bank`: open pipeline staging (erasing inactive bank) and
/// prime the streaming SHA accumulator.
fn prepareBank(manifest: *const impl.Manifest) u16 {
    if (manifest.image_size_bytes > 0) {
        const begin_err = sess.stageBegin(&s_pipeline.session, manifest.image_size_bytes);
        if (begin_err != impl.err.ok) return begin_err;
    } else {
        const erase_err = g_ra8_ota_cfg.flash.erase.?(
            g_ra8_ota_cfg.flash.ctx,
            g_ra8_ota_cfg.flash.inactive_bank_addr,
            0,
        );
        if (erase_err != impl.err.ok) return erase_err;
    }
    return g_ra8_ota_cfg.crypto.sha256_init.?(g_ra8_ota_cfg.crypto.ctx);
}

/// `internal_download_loop`: chunk until the image is complete, bounded by
/// `impl.max_chunks` (NASA Rule 2).
fn downloadLoop(manifest: *const impl.Manifest) u16 {
    var chunks: u32 = 0;
    var e: u16 = impl.err.ok;
    while (s_bytes_done < manifest.image_size_bytes) {
        // Validation caps image_size at k_ra8_ota_max_image_bytes == 128 chunks,
        // so the budget is never actually exhausted.
        if (chunks >= impl.max_chunks) {
            e = impl.err.hw_error;
            break;
        }
        e = downloadChunk(manifest.image_size_bytes);
        if (e != impl.err.ok) break;
        chunks += 1;
    }
    return e;
}

pub export fn ra8_ota_download_to_inactive_bank(
    manifest_or_null: ?*const impl.Manifest,
) callconv(.c) u16 {
    if (!g_ra8_ota_initialized) return impl.err.not_initialized;
    const manifest = manifest_or_null orelse return refuseNull("manifest");
    if (impl.downloadStateInvalid(impl.state.idle, impl.state.downloading, g_ra8_ota_state)) {
        return impl.err.invalid_state;
    }
    if (impl.imageExceedsBank(manifest.image_size_bytes, g_ra8_ota_cfg.flash.bank_size_bytes)) {
        priv_ota_set_state(impl.state.failed, impl.err.invalid_size);
        return impl.err.invalid_size;
    }

    if (impl.freshDownload(s_bytes_done)) {
        const prepared = prepareBank(manifest);
        if (prepared != impl.err.ok) {
            priv_ota_set_state(impl.state.failed, prepared);
            return prepared;
        }
    }

    var content_len: u32 = 0;
    var e = g_ra8_ota_cfg.net.open.?(
        g_ra8_ota_cfg.net.ctx,
        @ptrCast(&manifest.image_url),
        &content_len,
    );
    if (e != impl.err.ok) {
        priv_ota_set_state(impl.state.failed, e);
        return e;
    }
    priv_ota_set_state(impl.state.downloading, impl.err.ok);

    e = downloadLoop(manifest);
    _ = g_ra8_ota_cfg.net.close.?(g_ra8_ota_cfg.net.ctx);

    if (e != impl.err.ok) {
        priv_ota_set_state(impl.state.failed, e);
        return e;
    }
    if (manifest.image_size_bytes > 0) {
        const fin_err = sess.stageFinish(&s_pipeline.session);
        if (fin_err != impl.err.ok) {
            priv_ota_set_state(impl.state.failed, fin_err);
            return fin_err;
        }
    }
    priv_ota_set_state(impl.state.verifying, impl.err.ok);
    return impl.err.ok;
}

// =============================================================================
// Public API: verify (shared ra8_update pipeline)
// =============================================================================

pub export fn ra8_ota_verify_signature(
    manifest_or_null: ?*const impl.Manifest,
) callconv(.c) u16 {
    if (!g_ra8_ota_initialized) return impl.err.not_initialized;
    const manifest = manifest_or_null orelse return refuseNull("manifest");
    if (g_ra8_ota_state != impl.state.verifying) return impl.err.invalid_state;

    s_pipeline.session.cfg.pubkey_handle = g_ra8_ota_cfg.pubkey_handle;

    if (s_pipeline.bundle_mode) {
        var hdr: bn.Header = undefined;
        var descs: [bn.max_images]bn.ImageDesc = undefined;
        const b_err = bn.verifyBundleSession(
            &s_pipeline.session,
            0,
            &g_ra8_ota_buf,
            &hdr,
            &descs,
        );
        if (b_err != impl.err.ok) {
            priv_ota_set_state(impl.state.failed, b_err);
            return b_err;
        }
        priv_ota_set_state(impl.state.committing, impl.err.ok);
        return impl.err.ok;
    }

    if (s_pipeline.session.stage != @backingInt(pl.Stage.staged)) {
        s_pipeline.session.stage = @backingInt(pl.Stage.staged);
        s_pipeline.session.expected_bytes = manifest.image_size_bytes;
        s_pipeline.session.bytes_written = manifest.image_size_bytes;
        s_pipeline.session.verified = 0;
        s_pipeline.session.cfg.decl.payload = @backingInt(pl.PayloadKind.manifest_raw);
    }

    const size_le = impl.sizeLe(manifest.image_size_bytes);
    const meta_len = impl.version_str_bytes + impl.url_max_bytes + impl.size_field_bytes;
    var meta_prefix: [meta_len]u8 = undefined;
    @memcpy(meta_prefix[0..impl.version_str_bytes], &manifest.version);
    @memcpy(meta_prefix[impl.version_str_bytes..][0..impl.url_max_bytes], &manifest.image_url);
    @memcpy(
        meta_prefix[impl.version_str_bytes + impl.url_max_bytes ..][0..impl.size_field_bytes],
        &size_le,
    );

    const req = sess.VerifyReq{
        .expected_version = 1,
        .sig_len = manifest.signature_len,
        .meta_prefix_len = meta_len,
        .reserved0 = 0,
        .meta_prefix = &meta_prefix,
        .expected_sha256 = &manifest.image_sha256,
        .sig = @ptrCast(&manifest.signature),
    };
    const v_err = sess.verify(&s_pipeline.session, &req, &g_ra8_ota_buf);
    if (v_err != impl.err.ok) {
        const out_err: u16 = if (v_err == Err.validation_failed.raw())
            impl.err.hw_error
        else
            v_err;
        priv_ota_set_state(impl.state.failed, out_err);
        return out_err;
    }
    priv_ota_set_state(impl.state.committing, impl.err.ok);
    return impl.err.ok;
}

// =============================================================================
// Public API: commit and the step driver
// =============================================================================

pub export fn ra8_ota_commit_and_reboot() callconv(.c) u16 {
    if (!g_ra8_ota_initialized) return impl.err.not_initialized;
    if (g_ra8_ota_state != impl.state.committing) return impl.err.invalid_state;
    if (s_pipeline.session.stage != @backingInt(pl.Stage.verified)) {
        s_pipeline.session.stage = @backingInt(pl.Stage.verified);
        s_pipeline.session.verified = 1;
        s_pipeline.session.verified_version = 1;
    }
    const e = sess.commit(&s_pipeline.session, g_ra8_ota_cfg.flash.inactive_bank_index);
    if (e != impl.err.ok) {
        priv_ota_set_state(impl.state.failed, e);
        return e;
    }
    s_bytes_done = 0;
    priv_ota_set_state(impl.state.done, impl.err.ok);
    // On hardware this hook is overridden to call NVIC_SystemReset; the host
    // build keeps the weak no-op below.
    ra8_ota_system_reset_hook();
    return impl.err.ok;
}

/// `internal_step_dispatch`: one transition, chosen by `impl.stepAction`.
fn stepDispatch() u16 {
    return switch (impl.stepAction(g_ra8_ota_state, s_manifest_valid)) {
        .check => blk: {
            var m: impl.Manifest = undefined;
            break :blk ra8_ota_check_for_update(&m);
        },
        .download => ra8_ota_download_to_inactive_bank(&s_manifest),
        .verify => ra8_ota_verify_signature(&s_manifest),
        .commit => ra8_ota_commit_and_reboot(),
        .refuse => impl.err.invalid_state,
        .settle => impl.err.ok,
    };
}

pub export fn ra8_ota_run_step() callconv(.c) u16 {
    if (!g_ra8_ota_initialized) return impl.err.not_initialized;
    return stepDispatch();
}

pub export fn ra8_ota_run_full_update() callconv(.c) u16 {
    if (!g_ra8_ota_initialized) return impl.err.not_initialized;
    // Bounded by the longest legal sequence (idle -> checking -> downloading ->
    // verifying -> committing -> done), padded to the state count.
    var i: u32 = 0;
    while (i < impl.state.count) : (i += 1) {
        if (impl.isTerminal(g_ra8_ota_state)) break;
        const e = ra8_ota_run_step();
        if (e != impl.err.ok) return e;
    }
    return s_last_err;
}

// =============================================================================
// Weak system-reset hook
// =============================================================================
//
// The weak default lives in `src/reset_hook.zig`, its own translation unit, so
// a strong definition in the image (or in `tests/misc/src/test_ra8_ota.c`)
// keeps overriding it: the linker leaves that archive member behind and the
// call above binds to the override. Defining it here instead would make the
// call resolve inside this object and the override would never be reached.
