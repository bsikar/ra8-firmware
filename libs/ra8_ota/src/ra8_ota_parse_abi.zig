//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the pure parsing/validation cluster of `ra8_ota`.
//!
//! Exports the five promoted symbols `src/ra8_ota_internal.h` declares for this
//! cluster, with the C's guard order and log lines preserved byte for byte.
//! The orchestration TU (`ra8_ota.c`) and the verify cluster
//! (`ra8_ota_verify.c`) are deliberately still C on this branch: they own every
//! mutable module static, so they call into this archive and never the other
//! way round.

const std = @import("std");
const impl = @import("internal/root.zig");

/// Re-exported so the ABI tests share these exact types with the membrane.
pub const internal = impl;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// The C duplicates this immutable literal in every ra8_ota TU.
const tag: [*:0]const u8 = "ra8_ota";

/// `RA8_CHECK_NULL_PTR(ptr, s_tag, message)`: log then answer null_ptr.
fn refuseNull(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return impl.err.null_ptr;
}

// =============================================================================
// The two promoted MC/DC predicates
// =============================================================================

pub export fn priv_ota_char_in_range(c: c_char, lo: c_char, hi: c_char) callconv(.c) bool {
    return impl.charInRange(c, lo, hi);
}

pub export fn priv_ota_download_state_invalid(
    state_idle_val: u32,
    state_downloading_val: u32,
    state: u32,
) callconv(.c) bool {
    return impl.downloadStateInvalid(state_idle_val, state_downloading_val, state);
}

// =============================================================================
// Configuration validation
// =============================================================================

/// `internal_validate_cfg_net`.
fn validateNet(cfg: *const impl.Cfg) u16 {
    if (cfg.net.open == null) return refuseNull("net.open");
    if (cfg.net.read == null) return refuseNull("net.read");
    if (cfg.net.close == null) return refuseNull("net.close");
    return impl.err.ok;
}

/// `internal_validate_cfg_crypto`.
fn validateCrypto(cfg: *const impl.Cfg) u16 {
    if (cfg.crypto.sha256_init == null) return refuseNull("crypto.sha256_init");
    if (cfg.crypto.sha256_update == null) return refuseNull("crypto.sha256_update");
    if (cfg.crypto.sha256_final == null) return refuseNull("crypto.sha256_final");
    if (cfg.crypto.ecdsa_verify == null) return refuseNull("crypto.ecdsa_verify");
    return impl.err.ok;
}

/// `internal_validate_cfg_flash`. The two size gates emit no log line.
fn validateFlash(cfg: *const impl.Cfg) u16 {
    if (cfg.flash.erase == null) return refuseNull("flash.erase");
    if (cfg.flash.program == null) return refuseNull("flash.program");
    if (cfg.flash.set_startup == null) return refuseNull("flash.set_startup");
    if (cfg.flash.readback == null) return refuseNull("flash.readback");
    return impl.bankSizeStatus(cfg.flash.bank_size_bytes);
}

pub export fn priv_ota_validate_cfg(cfg_or_null: ?*const impl.Cfg) callconv(.c) u16 {
    const cfg = cfg_or_null orelse return refuseNull("cfg");

    const net_status = validateNet(cfg);
    if (net_status != impl.err.ok) return net_status;

    const crypto_status = validateCrypto(cfg);
    if (crypto_status != impl.err.ok) return crypto_status;

    const flash_status = validateFlash(cfg);
    if (flash_status != impl.err.ok) return flash_status;

    if (impl.manifestUrlEmpty(cfg.manifest_url[0])) return impl.err.invalid_arg;
    return impl.err.ok;
}

// =============================================================================
// JSON scanning
// =============================================================================

pub export fn priv_ota_json_u32(
    json: [*:0]const u8,
    key: [*:0]const u8,
    out_v: *u32,
) callconv(.c) u16 {
    return impl.jsonU32(json, key, out_v);
}

// =============================================================================
// Manifest decode
// =============================================================================

/// `internal_manifest_decode_crypto`: the sha256 and signature hex blobs.
fn decodeCrypto(json: [*:0]const u8, out: *impl.Manifest) u16 {
    var hex: [impl.hex_buf_bytes]u8 = undefined;
    const hex_ptr: [*:0]const u8 = @ptrCast(&hex);

    const digest_status = impl.jsonStr(json, "\"sha256\"", hex[0..]);
    if (digest_status != impl.err.ok) return digest_status;
    const decoded_digest = impl.hexDecode(hex_ptr, out.image_sha256[0..]);
    if (decoded_digest != impl.sha256_bytes) return impl.err.invalid_arg;

    const signature_status = impl.jsonStr(json, "\"signature\"", hex[0..]);
    if (signature_status != impl.err.ok) return signature_status;
    const decoded_signature = impl.hexDecode(hex_ptr, out.signature[0..]);
    if (decoded_signature == 0) return impl.err.invalid_arg;

    out.signature_len = @intCast(decoded_signature);
    return impl.err.ok;
}

pub export fn priv_ota_manifest_decode(
    json: [*:0]const u8,
    out: *impl.Manifest,
) callconv(.c) u16 {
    @memset(std.mem.asBytes(out), 0);

    const version_status = impl.jsonStr(json, "\"version\"", out.version[0..]);
    if (version_status != impl.err.ok) return version_status;

    const url_status = impl.jsonStr(json, "\"url\"", out.image_url[0..]);
    if (url_status != impl.err.ok) return url_status;

    const size_status = impl.jsonU32(json, "\"size\"", &out.image_size_bytes);
    if (size_status != impl.err.ok) return size_status;

    const bound_status = impl.manifestSizeStatus(out.image_size_bytes);
    if (bound_status != impl.err.ok) return bound_status;

    return decodeCrypto(json, out);
}
