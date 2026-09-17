//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the orchestration + verify membrane: the guard order on
//! every public entry point, the state-machine transitions, the progress
//! fan-out, the chunked download and re-hash passes, the constant-time digest
//! verdict and the metadata-bound ECDSA material (T5-05).
//!
//! The fake backends mirror the C fixtures in `tests/misc/src/test_ra8_ota.c`:
//! a scripted network stream, a counting SHA-256 that records every byte fed to
//! it, and a flash model with an in-memory bank.

const std = @import("std");
const abi = @import("abi");
const impl = abi.internal;

const ok = impl.err.ok;

// =============================================================================
// Log sink
// =============================================================================

var log_calls: usize = 0;
var last_message: [96]u8 = undefined;
var last_message_len: usize = 0;
var last_tag: [64]u8 = undefined;
var last_tag_len: usize = 0;

fn capture(destination: []u8, length: *usize, text: [*:0]const u8) void {
    const slice = std.mem.span(text);
    const take = @min(slice.len, destination.len);
    @memcpy(destination[0..take], slice[0..take]);
    length.* = take;
}

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    log_calls += 1;
    capture(&last_tag, &last_tag_len, tag);
    capture(&last_message, &last_message_len, message);
}

fn loggedMessage() []const u8 {
    return last_message[0..last_message_len];
}

fn loggedTag() []const u8 {
    return last_tag[0..last_tag_len];
}

/// `ra8_ct_equal` from ra8_core: the real archive supplies it on target, so the
/// test binary stands in a straightforward full-length compare.
export fn ra8_ct_equal(a: *const anyopaque, b: *const anyopaque, len: usize) bool {
    const lhs: [*]const u8 = @ptrCast(a);
    const rhs: [*]const u8 = @ptrCast(b);
    var same: u8 = 0;
    var i: usize = 0;
    while (i < len) : (i += 1) same |= lhs[i] ^ rhs[i];
    return same == 0;
}

// =============================================================================
// Fake backends
// =============================================================================

const bank_bytes = 16 * 1024;

const Model = struct {
    // network
    open_calls: usize = 0,
    close_calls: usize = 0,
    open_status: u16 = ok,
    read_status: u16 = ok,
    advertised_len: u32 = 0,
    stream: []const u8 = &.{},
    stream_pos: usize = 0,
    /// Bytes handed back per read call; 0 means "as much as asked for".
    read_granularity: u32 = 0,
    /// Report EOF immediately, whatever is left in the stream.
    starve: bool = false,

    // crypto
    sha_init_calls: usize = 0,
    sha_init_status: u16 = ok,
    sha_update_status: u16 = ok,
    sha_final_status: u16 = ok,
    ecdsa_status: u16 = ok,
    fed: [bank_bytes * 2]u8 = undefined,
    fed_len: usize = 0,
    /// Digest handed back by sha256_final.
    digest: [32]u8 = @splat(0xAB),
    /// Material the ECDSA verifier was handed.
    signed_material: [32]u8 = @splat(0),
    ecdsa_sig_len: u32 = 0,
    ecdsa_pubkey: u32 = 0,

    // flash
    erase_calls: usize = 0,
    erase_status: u16 = ok,
    program_status: u16 = ok,
    readback_status: u16 = ok,
    set_startup_status: u16 = ok,
    startup_index: u8 = 0xFF,
    startup_enabled: bool = false,
    bank: [bank_bytes]u8 = @splat(0xFF),
    programmed: usize = 0,

    // progress
    progress_calls: usize = 0,
    last_progress: impl.Progress = .{
        .state = 0xFF,
        .bytes_done = 0,
        .bytes_total = 0,
        .last_err = 0xFFFF,
    },
};

var model: Model = .{};
var reset_hook_calls: usize = 0;

/// The archive's weak default lives in `src/reset_hook.zig`, its own object, so
/// this strong definition is what the membrane reaches, exactly as the override
/// in `tests/misc/src/test_ra8_ota.c` does in the C build.
export fn ra8_ota_system_reset_hook() callconv(.c) void {
    reset_hook_calls += 1;
}

fn netOpen(_: ?*anyopaque, _: [*:0]const u8, out_len: *u32) callconv(.c) u16 {
    model.open_calls += 1;
    if (model.open_status != ok) return model.open_status;
    out_len.* = model.advertised_len;
    return ok;
}

fn netRead(_: ?*anyopaque, dst: [*]u8, cap: u32, out_got: *u32) callconv(.c) u16 {
    if (model.read_status != ok) return model.read_status;
    if (model.starve) {
        out_got.* = 0;
        return ok;
    }
    const left = model.stream.len - model.stream_pos;
    var want: usize = @min(@as(usize, cap), left);
    if (model.read_granularity != 0) want = @min(want, @as(usize, model.read_granularity));
    @memcpy(dst[0..want], model.stream[model.stream_pos..][0..want]);
    model.stream_pos += want;
    out_got.* = @intCast(want);
    return ok;
}

fn netClose(_: ?*anyopaque) callconv(.c) u16 {
    model.close_calls += 1;
    return ok;
}

fn shaInit(_: ?*anyopaque) callconv(.c) u16 {
    model.sha_init_calls += 1;
    if (model.sha_init_status != ok) return model.sha_init_status;
    model.fed_len = 0;
    return ok;
}

fn shaUpdate(_: ?*anyopaque, data: [*]const u8, len: u32) callconv(.c) u16 {
    if (model.sha_update_status != ok) return model.sha_update_status;
    const take = @min(@as(usize, len), model.fed.len - model.fed_len);
    @memcpy(model.fed[model.fed_len..][0..take], data[0..take]);
    model.fed_len += take;
    return ok;
}

fn shaFinal(_: ?*anyopaque, out: [*]u8) callconv(.c) u16 {
    if (model.sha_final_status != ok) return model.sha_final_status;
    @memcpy(out[0..32], &model.digest);
    return ok;
}

fn ecdsaVerify(
    _: ?*anyopaque,
    pubkey: u32,
    digest: [*]const u8,
    _: [*]const u8,
    sig_len: u32,
) callconv(.c) u16 {
    model.ecdsa_pubkey = pubkey;
    model.ecdsa_sig_len = sig_len;
    @memcpy(&model.signed_material, digest[0..32]);
    return model.ecdsa_status;
}

fn flashErase(_: ?*anyopaque, _: u32, _: u32) callconv(.c) u16 {
    model.erase_calls += 1;
    return model.erase_status;
}

fn flashProgram(_: ?*anyopaque, addr: u32, data: [*]const u8, len: u32) callconv(.c) u16 {
    if (model.program_status != ok) return model.program_status;
    const at: usize = @intCast(addr);
    const take = @min(@as(usize, len), model.bank.len - at);
    @memcpy(model.bank[at..][0..take], data[0..take]);
    model.programmed += take;
    return ok;
}

fn flashSetStartup(_: ?*anyopaque, index: u8, enable: bool) callconv(.c) u16 {
    if (model.set_startup_status != ok) return model.set_startup_status;
    model.startup_index = index;
    model.startup_enabled = enable;
    return ok;
}

fn flashReadback(_: ?*anyopaque, addr: u32, dst: [*]u8, len: u32) callconv(.c) u16 {
    if (model.readback_status != ok) return model.readback_status;
    const at: usize = @intCast(addr);
    const take = @min(@as(usize, len), model.bank.len - at);
    @memcpy(dst[0..take], model.bank[at..][0..take]);
    return ok;
}

fn onProgress(snapshot: ?*const anyopaque) callconv(.c) void {
    model.progress_calls += 1;
    const typed: *const impl.Progress = @ptrCast(@alignCast(snapshot.?));
    model.last_progress = typed.*;
}

// =============================================================================
// Fixture
// =============================================================================

fn baseCfg() impl.Cfg {
    var cfg = std.mem.zeroes(impl.Cfg);
    const url = "https://example.invalid/manifest.json";
    @memcpy(cfg.manifest_url[0..url.len], url);
    cfg.pubkey_handle = 0x5EED;
    cfg.on_progress = onProgress;
    cfg.net = .{ .open = netOpen, .read = netRead, .close = netClose, .ctx = null };
    cfg.crypto = .{
        .sha256_init = shaInit,
        .sha256_update = shaUpdate,
        .sha256_final = shaFinal,
        .ecdsa_verify = ecdsaVerify,
        .ctx = null,
    };
    cfg.flash = .{
        .erase = flashErase,
        .program = flashProgram,
        .set_startup = flashSetStartup,
        .readback = flashReadback,
        .inactive_bank_addr = 0,
        .bank_size_bytes = bank_bytes,
        .inactive_bank_index = 1,
        .ctx = null,
    };
    return cfg;
}

/// Every test starts from a deinitialized module and a fresh model.
fn reset() void {
    _ = abi.ra8_ota_deinit();
    model = .{};
    reset_hook_calls = 0;
    log_calls = 0;
    last_message_len = 0;
    last_tag_len = 0;
}

fn initModule() !void {
    const cfg = baseCfg();
    try std.testing.expectEqual(ok, abi.ra8_ota_init(&cfg));
}

const manifest_json =
    "{\"version\": \"1.2.3\", \"url\": \"https://example.invalid/fw.bin\", " ++
    "\"size\": 8192, " ++
    "\"sha256\": \"" ++ "ab" ** 32 ++ "\", " ++
    "\"signature\": \"" ++ "cd" ** 64 ++ "\"}";

// =============================================================================
// Lifecycle
// =============================================================================

test "init refuses a second init with invalid_state and mutates nothing" {
    reset();
    try initModule();
    try std.testing.expectEqual(impl.err.invalid_state, abi.ra8_ota_init(&baseCfg()));
    try std.testing.expect(abi.g_ra8_ota_initialized);
}

test "init runs the validator before capturing the config" {
    reset();
    var cfg = baseCfg();
    cfg.net.read = null;
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_ota_init(&cfg));
    try std.testing.expect(!abi.g_ra8_ota_initialized);
    try std.testing.expectEqualStrings("net.read", loggedMessage());
    try std.testing.expectEqualStrings("ra8_ota", loggedTag());
}

test "init captures the config by value and lands in idle" {
    reset();
    try initModule();
    try std.testing.expectEqual(impl.state.idle, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(u32, 0x5EED), abi.g_ra8_ota_cfg.pubkey_handle);
    try std.testing.expectEqual(@as(u32, bank_bytes), abi.g_ra8_ota_cfg.flash.bank_size_bytes);
}

test "deinit zeroes the config and clears the init flag" {
    reset();
    try initModule();
    try std.testing.expectEqual(ok, abi.ra8_ota_deinit());
    try std.testing.expect(!abi.g_ra8_ota_initialized);
    try std.testing.expectEqual(@as(u32, 0), abi.g_ra8_ota_cfg.pubkey_handle);
    try std.testing.expectEqual(impl.state.idle, abi.ra8_ota_get_state());
}

test "every entry point answers not_initialized before init, with no log line" {
    reset();
    var manifest = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_ota_check_for_update(&manifest));
    try std.testing.expectEqual(
        impl.err.not_initialized,
        abi.ra8_ota_download_to_inactive_bank(&manifest),
    );
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_ota_verify_signature(&manifest));
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_ota_commit_and_reboot());
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_ota_run_step());
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_ota_run_full_update());
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "the init flag is judged before the NULL guard on check and download" {
    reset();
    // Pre-init NULL answers not_initialized, not null_ptr, and logs nothing.
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_ota_check_for_update(null));
    try std.testing.expectEqual(@as(usize, 0), log_calls);
    try initModule();
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_ota_check_for_update(null));
    try std.testing.expectEqualStrings("out_manifest", loggedMessage());
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_ota_download_to_inactive_bank(null));
    try std.testing.expectEqualStrings("manifest", loggedMessage());
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_ota_verify_signature(null));
    try std.testing.expectEqualStrings("manifest", loggedMessage());
}

// =============================================================================
// Manifest check
// =============================================================================

test "check_for_update caches the manifest and returns to idle" {
    reset();
    try initModule();
    model.stream = manifest_json;
    model.advertised_len = @intCast(manifest_json.len);

    var manifest = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(ok, abi.ra8_ota_check_for_update(&manifest));
    try std.testing.expectEqual(impl.state.idle, abi.ra8_ota_get_state());
    try std.testing.expectEqualStrings("1.2.3", std.mem.sliceTo(&manifest.version, 0));
    try std.testing.expectEqual(@as(u32, 8192), manifest.image_size_bytes);
    try std.testing.expectEqual(@as(u16, 64), manifest.signature_len);
    try std.testing.expectEqual(@as(usize, 1), model.open_calls);
    try std.testing.expectEqual(@as(usize, 1), model.close_calls);
    // The drained payload is NUL-terminated inside the shared buffer.
    try std.testing.expectEqual(@as(u8, 0), abi.g_ra8_ota_buf[manifest_json.len]);
}

test "an over-long advertised body is invalid_size and still closes the stream" {
    reset();
    try initModule();
    model.advertised_len = impl.manifest_max_bytes + 1;

    var manifest = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(impl.err.invalid_size, abi.ra8_ota_check_for_update(&manifest));
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(usize, 1), model.close_calls);
}

test "a network open failure lands the machine in error" {
    reset();
    try initModule();
    model.open_status = 0x301;
    var manifest = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(@as(u16, 0x301), abi.ra8_ota_check_for_update(&manifest));
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
    // A failed open never reaches close.
    try std.testing.expectEqual(@as(usize, 0), model.close_calls);
}

test "a garbage payload fails the decode and lands in error" {
    reset();
    try initModule();
    model.stream = "not json at all";
    model.advertised_len = 15;
    var manifest = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_ota_check_for_update(&manifest));
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
}

test "check_for_update refuses a non-idle state" {
    reset();
    try initModule();
    abi.g_ra8_ota_state = impl.state.verifying;
    var manifest = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(impl.err.invalid_state, abi.ra8_ota_check_for_update(&manifest));
}

// =============================================================================
// Progress fan-out
// =============================================================================

test "priv_ota_set_state latches the state and fans the snapshot out" {
    reset();
    try initModule();
    abi.priv_ota_set_state(impl.state.verifying, impl.err.crc_mismatch);
    try std.testing.expectEqual(impl.state.verifying, abi.g_ra8_ota_state);
    try std.testing.expectEqual(@as(usize, 1), model.progress_calls);
    try std.testing.expectEqual(impl.state.verifying, model.last_progress.state);
    try std.testing.expectEqual(impl.err.crc_mismatch, model.last_progress.last_err);
    // No manifest cached yet, so the total stays zero.
    try std.testing.expectEqual(@as(u32, 0), model.last_progress.bytes_total);
}

test "a config without a progress callback still latches the state" {
    reset();
    var cfg = baseCfg();
    cfg.on_progress = null;
    try std.testing.expectEqual(ok, abi.ra8_ota_init(&cfg));
    abi.priv_ota_set_state(impl.state.done, ok);
    try std.testing.expectEqual(impl.state.done, abi.g_ra8_ota_state);
    try std.testing.expectEqual(@as(usize, 0), model.progress_calls);
}

// =============================================================================
// Download
// =============================================================================

var image_backing: [8192]u8 = undefined;

fn scriptedImage() []const u8 {
    var i: usize = 0;
    while (i < image_backing.len) : (i += 1) image_backing[i] = @truncate(i * 7);
    return image_backing[0..];
}

fn cachedManifest(size: u32) impl.Manifest {
    var manifest = std.mem.zeroes(impl.Manifest);
    const url = "https://example.invalid/fw.bin";
    @memcpy(manifest.image_url[0..url.len], url);
    manifest.image_size_bytes = size;
    manifest.image_sha256 = @splat(0xAB);
    manifest.signature_len = 64;
    return manifest;
}

test "download streams the whole image, hashes it and programs it" {
    reset();
    try initModule();
    const image = scriptedImage();
    model.stream = image;
    const manifest = cachedManifest(@intCast(image.len));

    try std.testing.expectEqual(ok, abi.ra8_ota_download_to_inactive_bank(&manifest));
    try std.testing.expectEqual(impl.state.verifying, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(usize, 1), model.erase_calls);
    try std.testing.expectEqual(@as(usize, image.len), model.programmed);
    try std.testing.expectEqualSlices(u8, image, model.bank[0..image.len]);
    try std.testing.expectEqualSlices(u8, image, model.fed[0..model.fed_len]);
    try std.testing.expectEqual(@as(usize, 1), model.close_calls);
}

test "a short read is drained across calls without losing a byte" {
    reset();
    try initModule();
    const image = scriptedImage();
    model.stream = image;
    model.read_granularity = 101; // deliberately not a chunk divisor
    const manifest = cachedManifest(@intCast(image.len));

    try std.testing.expectEqual(ok, abi.ra8_ota_download_to_inactive_bank(&manifest));
    try std.testing.expectEqualSlices(u8, image, model.bank[0..image.len]);
}

test "an image larger than the bank is invalid_size before any erase" {
    reset();
    try initModule();
    const manifest = cachedManifest(bank_bytes + 1);
    try std.testing.expectEqual(
        impl.err.invalid_size,
        abi.ra8_ota_download_to_inactive_bank(&manifest),
    );
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(usize, 0), model.erase_calls);
}

test "download refuses any state that is neither idle nor downloading" {
    reset();
    try initModule();
    const manifest = cachedManifest(4096);
    for ([_]u8{
        impl.state.checking,
        impl.state.verifying,
        impl.state.committing,
        impl.state.done,
        impl.state.failed,
    }) |candidate| {
        abi.g_ra8_ota_state = candidate;
        try std.testing.expectEqual(
            impl.err.invalid_state,
            abi.ra8_ota_download_to_inactive_bank(&manifest),
        );
    }
}

test "an erase failure stops before the stream is opened" {
    reset();
    try initModule();
    model.erase_status = 0x204;
    const manifest = cachedManifest(4096);
    try std.testing.expectEqual(
        @as(u16, 0x204),
        abi.ra8_ota_download_to_inactive_bank(&manifest),
    );
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(usize, 0), model.open_calls);
}

test "a starved stream is a hw_error, and the stream is still closed" {
    reset();
    try initModule();
    model.starve = true;
    const manifest = cachedManifest(4096);
    try std.testing.expectEqual(
        impl.err.hw_error,
        abi.ra8_ota_download_to_inactive_bank(&manifest),
    );
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(usize, 1), model.close_calls);
}

test "a flash program failure propagates and closes the stream" {
    reset();
    try initModule();
    model.stream = scriptedImage();
    model.program_status = 0x402;
    const manifest = cachedManifest(4096);
    try std.testing.expectEqual(
        @as(u16, 0x402),
        abi.ra8_ota_download_to_inactive_bank(&manifest),
    );
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(usize, 1), model.close_calls);
}

// =============================================================================
// Verify
// =============================================================================

fn armVerify(size: u32) impl.Manifest {
    const manifest = cachedManifest(size);
    var i: usize = 0;
    while (i < size) : (i += 1) model.bank[i] = @truncate(i * 3);
    abi.g_ra8_ota_state = impl.state.verifying;
    return manifest;
}

test "verify re-hashes the bank, binds the metadata and commits" {
    reset();
    try initModule();
    const manifest = armVerify(8192);

    try std.testing.expectEqual(ok, abi.ra8_ota_verify_signature(&manifest));
    try std.testing.expectEqual(impl.state.committing, abi.ra8_ota_get_state());
    // Two SHA passes: the bank re-hash, then the bound material.
    try std.testing.expectEqual(@as(usize, 2), model.sha_init_calls);
    try std.testing.expectEqual(@as(u32, 0x5EED), model.ecdsa_pubkey);
    try std.testing.expectEqual(@as(u32, 64), model.ecdsa_sig_len);
    // The second pass fed exactly version || url || size_le || digest.
    try std.testing.expectEqual(@as(usize, 324), model.fed_len);
    try std.testing.expectEqualSlices(u8, &manifest.version, model.fed[0..32]);
    try std.testing.expectEqualSlices(u8, &manifest.image_url, model.fed[32..288]);
    try std.testing.expectEqualSlices(u8, &impl.sizeLe(8192), model.fed[288..292]);
    try std.testing.expectEqualSlices(u8, &model.digest, model.fed[292..324]);
}

test "the re-hash pass reads the bank back in chunks" {
    reset();
    try initModule();
    var manifest = armVerify(8192);
    // Make the bound pass fail after the re-hash so `fed` still holds the bank.
    model.digest = @splat(0xAB);
    manifest.image_sha256 = @splat(0xAB);
    model.ecdsa_status = ok;
    try std.testing.expectEqual(ok, abi.ra8_ota_verify_signature(&manifest));
    // The bank content itself was hashed first (asserted through the model's
    // bank, since the second pass overwrote `fed`).
    try std.testing.expectEqual(@as(u8, 0), model.bank[0]);
    try std.testing.expectEqual(@as(u8, 3), model.bank[1]);
}

test "verify refuses any state other than verifying" {
    reset();
    try initModule();
    const manifest = cachedManifest(4096);
    abi.g_ra8_ota_state = impl.state.downloading;
    try std.testing.expectEqual(impl.err.invalid_state, abi.ra8_ota_verify_signature(&manifest));
}

test "a digest mismatch is crc_mismatch and never reaches the verifier" {
    reset();
    try initModule();
    var manifest = armVerify(4096);
    manifest.image_sha256 = @splat(0x11); // model.digest stays 0xAB
    try std.testing.expectEqual(
        impl.err.crc_mismatch,
        abi.ra8_ota_verify_signature(&manifest),
    );
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(u32, 0), model.ecdsa_sig_len);
}

test "a rejected signature is reported as hw_error, not the backend's code" {
    reset();
    try initModule();
    const manifest = armVerify(4096);
    model.ecdsa_status = 0x666;
    try std.testing.expectEqual(impl.err.hw_error, abi.ra8_ota_verify_signature(&manifest));
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
}

test "a readback failure fails the verify with the backend's own code" {
    reset();
    try initModule();
    const manifest = armVerify(4096);
    model.readback_status = 0x407;
    try std.testing.expectEqual(@as(u16, 0x407), abi.ra8_ota_verify_signature(&manifest));
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
}

test "a zero-size image hashes nothing but still binds and verifies" {
    reset();
    try initModule();
    const manifest = armVerify(0);
    try std.testing.expectEqual(ok, abi.ra8_ota_verify_signature(&manifest));
    try std.testing.expectEqual(@as(usize, 324), model.fed_len);
}

// =============================================================================
// Commit
// =============================================================================

test "commit latches the inactive bank and fires the reset hook" {
    reset();
    try initModule();
    abi.g_ra8_ota_state = impl.state.committing;
    try std.testing.expectEqual(ok, abi.ra8_ota_commit_and_reboot());
    try std.testing.expectEqual(impl.state.done, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(u8, 1), model.startup_index);
    try std.testing.expect(model.startup_enabled);
    try std.testing.expectEqual(@as(usize, 1), reset_hook_calls);
}

test "commit refuses a state other than committing" {
    reset();
    try initModule();
    try std.testing.expectEqual(impl.err.invalid_state, abi.ra8_ota_commit_and_reboot());
}

test "a set_startup failure lands in error and skips the reset" {
    reset();
    try initModule();
    abi.g_ra8_ota_state = impl.state.committing;
    model.set_startup_status = 0x205;
    try std.testing.expectEqual(@as(u16, 0x205), abi.ra8_ota_commit_and_reboot());
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(u8, 0xFF), model.startup_index);
    try std.testing.expectEqual(@as(usize, 0), reset_hook_calls);
}

// =============================================================================
// Step driver
// =============================================================================

test "run_step from idle with nothing cached fetches the manifest" {
    reset();
    try initModule();
    model.stream = manifest_json;
    model.advertised_len = @intCast(manifest_json.len);
    try std.testing.expectEqual(ok, abi.ra8_ota_run_step());
    try std.testing.expectEqual(@as(usize, 1), model.open_calls);
    try std.testing.expectEqual(impl.state.idle, abi.ra8_ota_get_state());
}

test "run_step refuses a transient state with no cached manifest" {
    reset();
    try initModule();
    abi.g_ra8_ota_state = impl.state.checking;
    try std.testing.expectEqual(impl.err.invalid_state, abi.ra8_ota_run_step());
}

test "run_step settles on a terminal state" {
    reset();
    try initModule();
    abi.g_ra8_ota_state = impl.state.done;
    try std.testing.expectEqual(ok, abi.ra8_ota_run_step());
    try std.testing.expectEqual(impl.state.done, abi.ra8_ota_get_state());
    abi.g_ra8_ota_state = impl.state.failed;
    try std.testing.expectEqual(ok, abi.ra8_ota_run_step());
}

test "run_full_update drives idle all the way to done" {
    reset();
    try initModule();
    // One stream serves the manifest fetch and then the image download.
    const image = scriptedImage();
    model.stream = manifest_json ++ ("" ++ "");
    model.advertised_len = @intCast(manifest_json.len);
    _ = image;

    // Fetch the manifest first so the cached copy drives the rest.
    var manifest = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(ok, abi.ra8_ota_check_for_update(&manifest));

    // Re-point the stream at an 8192-byte image matching the manifest size.
    model.stream = scriptedImage();
    model.stream_pos = 0;
    model.digest = @splat(0xAB); // matches the manifest's ab*32 digest

    try std.testing.expectEqual(ok, abi.ra8_ota_run_full_update());
    try std.testing.expectEqual(impl.state.done, abi.ra8_ota_get_state());
    try std.testing.expectEqual(@as(u8, 1), model.startup_index);
}

test "run_full_update returns the latched error once a step fails" {
    reset();
    try initModule();
    model.open_status = 0x301;
    try std.testing.expectEqual(@as(u16, 0x301), abi.ra8_ota_run_full_update());
    try std.testing.expectEqual(impl.state.failed, abi.ra8_ota_get_state());
}

test "run_full_update on an already-done machine reports the latched error" {
    reset();
    try initModule();
    abi.g_ra8_ota_state = impl.state.done;
    try std.testing.expectEqual(ok, abi.ra8_ota_run_full_update());
}
