//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure, state-free half of `ra8_ota`: the configuration validators, the
//! minimal JSON scanners, the hex decoder and the manifest field gates that
//! used to live in `src/ra8_ota_parse.c`.
//!
//! Nothing in this file touches module state and nothing here is `extern`:
//! the orchestration TU (`ra8_ota.c`) owns every mutable static, and the
//! `string.h` calls the C made (`strstr` / `strchr` / `strlen`) are
//! reimplemented here so the archive names no libc symbol.

const std = @import("std");

/// `ra8_err_t` values this cluster can return (C23 `enum : uint16_t`).
pub const err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const invalid_size: u16 = 0x105;
    pub const not_initialized: u16 = 0x10F;
    pub const hw_error: u16 = 0x204;
    pub const crc_mismatch: u16 = 0x405;
    pub const null_ptr: u16 = 0x504;
};

/// `ra8_ota_state_t` (C23 `enum : uint8_t`). `k_ra8_ota_state_error` is spelled
/// `failed` here because `error` is a Zig keyword; the numeric values are ABI.
pub const state = struct {
    pub const idle: u8 = 0;
    pub const checking: u8 = 1;
    pub const downloading: u8 = 2;
    pub const verifying: u8 = 3;
    pub const committing: u8 = 4;
    pub const done: u8 = 5;
    pub const failed: u8 = 6;
    pub const count: u8 = 7;
};

pub const chunk_bytes: u32 = 4096;
pub const manifest_max_bytes: u32 = 2048;
pub const sha256_bytes: u32 = 32;
pub const signature_max_bytes: u32 = 96;
pub const url_max_bytes: u32 = 256;
pub const version_str_bytes: u32 = 32;
pub const max_image_bytes: u32 = 0x80000;

/// Mirrors `ra8_ota_internal_const_t` in the C.
pub const json_skip_max: u32 = 8;
pub const u32_decimal_digits: u32 = 12;
pub const u32_decimal_base: u32 = 10;
pub const hex_alpha_offset: u8 = 10;
pub const hex_invalid_nibble: u8 = 0xFF;
pub const hex_chars_per_byte: u32 = 2;
pub const hex_nibble_shift: u3 = 4;
pub const hex_buf_bytes: usize = 257;

/// `ra8_ota_net_iface_t`.
pub const NetIface = extern struct {
    open: ?*const fn (?*anyopaque, [*:0]const u8, *u32) callconv(.c) u16,
    read: ?*const fn (?*anyopaque, [*]u8, u32, *u32) callconv(.c) u16,
    close: ?*const fn (?*anyopaque) callconv(.c) u16,
    ctx: ?*anyopaque,
};

/// `ra8_ota_crypto_iface_t`.
pub const CryptoIface = extern struct {
    sha256_init: ?*const fn (?*anyopaque) callconv(.c) u16,
    sha256_update: ?*const fn (?*anyopaque, [*]const u8, u32) callconv(.c) u16,
    sha256_final: ?*const fn (?*anyopaque, [*]u8) callconv(.c) u16,
    ecdsa_verify: ?*const fn (?*anyopaque, u32, [*]const u8, [*]const u8, u32) callconv(.c) u16,
    ctx: ?*anyopaque,
};

/// `ra8_ota_flash_iface_t`.
pub const FlashIface = extern struct {
    erase: ?*const fn (?*anyopaque, u32, u32) callconv(.c) u16,
    program: ?*const fn (?*anyopaque, u32, [*]const u8, u32) callconv(.c) u16,
    set_startup: ?*const fn (?*anyopaque, u8, bool) callconv(.c) u16,
    readback: ?*const fn (?*anyopaque, u32, [*]u8, u32) callconv(.c) u16,
    inactive_bank_addr: u32,
    bank_size_bytes: u32,
    inactive_bank_index: u8,
    ctx: ?*anyopaque,
};

/// `ra8_ota_cfg_t`.
pub const Cfg = extern struct {
    manifest_url: [url_max_bytes]u8,
    pubkey_handle: u32,
    on_progress: ?*const fn (?*const anyopaque) callconv(.c) void,
    net: NetIface,
    crypto: CryptoIface,
    flash: FlashIface,
    run_as_thread: bool,
};

/// `ra8_ota_manifest_t`.
pub const Manifest = extern struct {
    version: [version_str_bytes]u8,
    image_url: [url_max_bytes]u8,
    image_size_bytes: u32,
    image_sha256: [sha256_bytes]u8,
    signature: [signature_max_bytes]u8,
    signature_len: u16,
};

/// `ra8_ota_progress_t`: the snapshot `priv_ota_set_state` hands the caller's
/// progress callback.
pub const Progress = extern struct {
    state: u8,
    bytes_done: u32,
    bytes_total: u32,
    last_err: u16,
};

const ptr_bytes = @sizeOf(*anyopaque);
const ptr_align = @alignOf(*anyopaque);

fn after(offset: usize, comptime T: type) usize {
    return std.mem.alignForward(usize, offset, @alignOf(T)) + @sizeOf(T);
}

comptime {
    // Layouts are ABI: ra8_ota.c and three C suites see the same structs.
    std.debug.assert(@sizeOf(NetIface) == ptr_bytes * 4);
    std.debug.assert(@sizeOf(CryptoIface) == ptr_bytes * 5);

    std.debug.assert(@offsetOf(FlashIface, "inactive_bank_addr") == ptr_bytes * 4);
    std.debug.assert(@offsetOf(FlashIface, "bank_size_bytes") == (ptr_bytes * 4) + 4);
    std.debug.assert(@offsetOf(FlashIface, "inactive_bank_index") == (ptr_bytes * 4) + 8);
    std.debug.assert(@offsetOf(FlashIface, "ctx") ==
        std.mem.alignForward(usize, (ptr_bytes * 4) + 9, ptr_align));
    std.debug.assert(@sizeOf(FlashIface) ==
        std.mem.alignForward(usize, (ptr_bytes * 5) + 9, ptr_align));

    std.debug.assert(@offsetOf(Cfg, "manifest_url") == 0);
    std.debug.assert(@offsetOf(Cfg, "pubkey_handle") == url_max_bytes);
    std.debug.assert(@offsetOf(Cfg, "on_progress") ==
        std.mem.alignForward(usize, url_max_bytes + 4, ptr_align));
    std.debug.assert(@offsetOf(Cfg, "net") == @offsetOf(Cfg, "on_progress") + ptr_bytes);
    std.debug.assert(@offsetOf(Cfg, "crypto") == @offsetOf(Cfg, "net") + @sizeOf(NetIface));
    std.debug.assert(@offsetOf(Cfg, "flash") == @offsetOf(Cfg, "crypto") + @sizeOf(CryptoIface));
    std.debug.assert(@offsetOf(Cfg, "run_as_thread") ==
        @offsetOf(Cfg, "flash") + @sizeOf(FlashIface));

    std.debug.assert(@offsetOf(Manifest, "version") == 0);
    std.debug.assert(@offsetOf(Manifest, "image_url") == 32);
    std.debug.assert(@offsetOf(Manifest, "image_size_bytes") == 288);
    std.debug.assert(@offsetOf(Manifest, "image_sha256") == 292);
    std.debug.assert(@offsetOf(Manifest, "signature") == 324);
    std.debug.assert(@offsetOf(Manifest, "signature_len") == 420);
    std.debug.assert(@sizeOf(Manifest) == 424);

    std.debug.assert(@offsetOf(Progress, "state") == 0);
    std.debug.assert(@offsetOf(Progress, "bytes_done") == 4);
    std.debug.assert(@offsetOf(Progress, "bytes_total") == 8);
    std.debug.assert(@offsetOf(Progress, "last_err") == 12);
    std.debug.assert(@sizeOf(Progress) == 16);
}

// =============================================================================
// The two promoted MC/DC predicates
// =============================================================================

/// `(c >= lo) && (c <= hi)`, judged with the platform's own `char` signedness.
pub fn charInRange(c: c_char, lo: c_char, hi: c_char) bool {
    return (c >= lo) and (c <= hi);
}

/// `(state != idle) && (state != downloading)`. The candidate parameter is
/// spelled `state_val` because `state` is now a container declaration here.
pub fn downloadStateInvalid(state_idle_val: u32, state_downloading_val: u32, state_val: u32) bool {
    return (state_val != state_idle_val) and (state_val != state_downloading_val);
}

// =============================================================================
// string.h replacements, C semantics, indices instead of pointers
// =============================================================================

pub fn strLen(s: [*:0]const u8) usize {
    var n: usize = 0;
    while (s[n] != 0) : (n += 1) {}
    return n;
}

/// `strchr(s + from, c)` as an absolute index, or null when absent.
/// A `c` of 0 finds the terminator, exactly as C does.
pub fn chrFrom(s: [*:0]const u8, from: usize, c: u8) ?usize {
    var i = from;
    while (true) : (i += 1) {
        if (s[i] == c) return i;
        if (s[i] == 0) return null;
    }
}

/// `strstr(haystack, needle)` as an absolute index, or null when absent.
pub fn strStr(haystack: [*:0]const u8, needle: [*:0]const u8) ?usize {
    const needle_len = strLen(needle);
    if (needle_len == 0) return 0;
    var i: usize = 0;
    outer: while (true) : (i += 1) {
        var j: usize = 0;
        while (j < needle_len) : (j += 1) {
            if (haystack[i + j] == 0) return null;
            if (haystack[i + j] != needle[j]) continue :outer;
        }
        return i;
    }
}

// =============================================================================
// JSON / hex scanning
// =============================================================================

/// `internal_json_str`: copy the quoted value that follows `key` into `dst`,
/// NUL-terminated. `dst.len` is the C's `cap`.
pub fn jsonStr(json: [*:0]const u8, key: [*:0]const u8, dst: []u8) u16 {
    const hit = strStr(json, key) orelse return err.invalid_arg;
    const open = chrFrom(json, hit + strLen(key), '"') orelse return err.invalid_arg;
    const start = open + 1;
    const close = chrFrom(json, start, '"') orelse return err.invalid_arg;
    const n: u32 = @intCast(close - start);
    if (n + 1 > dst.len) return err.invalid_size;
    @memcpy(dst[0..n], json[start..close]);
    dst[n] = 0;
    return err.ok;
}

/// `priv_ota_json_u32`: bounded skip run, then up to 12 decimal digits.
/// The accumulate wraps exactly as the C's `uint32_t` arithmetic does.
pub fn jsonU32(json: [*:0]const u8, key: [*:0]const u8, out_v: *u32) u16 {
    const hit = strStr(json, key) orelse return err.invalid_arg;
    var p = hit + strLen(key);

    var guard: u32 = 0;
    while (guard < json_skip_max) : (guard += 1) {
        const c = json[p];
        if (c == ':' or c == ' ' or c == '"') {
            p += 1;
        } else {
            break;
        }
    }

    var v: u32 = 0;
    var i: u32 = 0;
    while (i < u32_decimal_digits) : (i += 1) {
        const c = json[p + i];
        if ((c < '0') or (c > '9')) break;
        v = (v *% u32_decimal_base) +% @as(u32, c - '0');
    }
    if (i == 0) return err.invalid_arg;
    out_v.* = v;
    return err.ok;
}

/// `internal_hex_nibble`: 0..15, or `hex_invalid_nibble` for a non-hex char.
pub fn hexNibble(c: c_char) u8 {
    if ((c >= '0') and (c <= '9')) return @intCast(c - '0');
    if ((c >= 'a') and (c <= 'f')) return hex_alpha_offset + @as(u8, @intCast(c - 'a'));
    if (charInRange(c, 'A', 'F')) return hex_alpha_offset + @as(u8, @intCast(c - 'A'));
    return hex_invalid_nibble;
}

/// `internal_hex_decode`: bytes written, or 0 on odd length, capacity
/// overflow or any non-hex character.
pub fn hexDecode(in: [*:0]const u8, out: []u8) u32 {
    const in_len: u32 = @intCast(strLen(in));
    if ((in_len % hex_chars_per_byte) != 0) return 0;
    const bytes = in_len / hex_chars_per_byte;
    if (bytes > out.len) return 0;

    var i: u32 = 0;
    while (i < bytes) : (i += 1) {
        const base_idx: usize = @as(usize, i) * hex_chars_per_byte;
        const hi = hexNibble(@bitCast(in[base_idx]));
        const lo = hexNibble(@bitCast(in[base_idx + 1]));
        if ((hi == hex_invalid_nibble) or (lo == hex_invalid_nibble)) return 0;
        out[i] = (hi << hex_nibble_shift) | lo;
    }
    return bytes;
}

// =============================================================================
// Configuration / manifest gates
// =============================================================================

/// `internal_validate_cfg_flash`'s two numeric gates, both invalid_arg.
pub fn bankSizeStatus(bank_size_bytes: u32) u16 {
    if (bank_size_bytes == 0) return err.invalid_arg;
    if (bank_size_bytes > max_image_bytes) return err.invalid_arg;
    return err.ok;
}

/// `priv_manifest_decode`'s size gates: zero is invalid_arg, over the cap is
/// invalid_size. Two different codes, deliberately.
pub fn manifestSizeStatus(image_size_bytes: u32) u16 {
    if (image_size_bytes == 0) return err.invalid_arg;
    if (image_size_bytes > max_image_bytes) return err.invalid_size;
    return err.ok;
}

/// The `cfg->manifest_url[0] == '\0'` gate.
pub fn manifestUrlEmpty(first_byte: u8) bool {
    return first_byte == 0;
}

// =============================================================================
// Orchestration / verify predicates and arithmetic
//
// The state machine itself lives in the ABI membrane (it mutates the module
// statics the C suites poke); everything here is the pure decision material
// that used to sit inline in `ra8_ota.c` / `ra8_ota_verify.c`.
// =============================================================================

/// `k_ra8_ota_manifest_max_bytes - 1U`: the drain cap the manifest fetch uses,
/// leaving room for the NUL the JSON scanners need.
pub const manifest_drain_cap: u32 = manifest_max_bytes - 1;

/// `(k_ra8_ota_max_image_bytes / k_ra8_ota_chunk_bytes) + 1U`: the static loop
/// bound both the download loop and the re-hash pass carry (NASA Rule 2).
pub const max_chunks: u32 = (max_image_bytes / chunk_bytes) + 1;

/// Fixed byte-widths bound into the OTA signature material (T5-05).
pub const size_field_bytes: u32 = 4;
pub const octet_bits: u5 = 8;

/// `total >= cap`: the drain loop's early exit before the next read.
pub fn drainFilled(total: u32, cap: u32) bool {
    return total >= cap;
}

/// `content_len > k_ra8_ota_manifest_max_bytes`: the advertised-length gate the
/// manifest fetch applies after a successful open (invalid_size, no log line).
pub fn manifestPayloadTooLarge(content_len: u32) bool {
    return content_len > manifest_max_bytes;
}

/// `(remaining < k_ra8_ota_chunk_bytes) ? remaining : k_ra8_ota_chunk_bytes`,
/// shared by the download chunk sizing and the re-hash pass.
pub fn chunkWant(remaining: u32) u32 {
    return if (remaining < chunk_bytes) remaining else chunk_bytes;
}

/// `manifest->image_size_bytes > g_ra8_ota_cfg.flash.bank_size_bytes`.
pub fn imageExceedsBank(image_size_bytes: u32, bank_size_bytes: u32) bool {
    return image_size_bytes > bank_size_bytes;
}

/// `s_bytes_done == 0U`: a fresh download start erases the bank and primes SHA.
pub fn freshDownload(bytes_done: u32) bool {
    return bytes_done == 0;
}

/// `bytes_total` for the progress snapshot: the cached size only once a
/// manifest has actually been decoded, else zero.
pub fn progressTotal(manifest_valid: bool, image_size_bytes: u32) u32 {
    return if (manifest_valid) image_size_bytes else 0;
}

/// `(state == done) || (state == error)`: `ra8_ota_run_full_update`'s stop gate.
pub fn isTerminal(s: u8) bool {
    return (s == state.done) or (s == state.failed);
}

/// The little-endian `image_size_bytes` segment of the signed material.
pub fn sizeLe(image_size_bytes: u32) [size_field_bytes]u8 {
    var out: [size_field_bytes]u8 = @splat(0);
    var i: u8 = 0;
    while (i < size_field_bytes) : (i += 1) {
        out[i] = @truncate(image_size_bytes >> @as(u5, @intCast(i)) * octet_bits);
    }
    return out;
}

/// What `internal_step_dispatch` resolves the current state to.
pub const StepAction = enum {
    /// Fetch and decode the manifest (idle, nothing cached yet).
    check,
    /// Download the cached manifest's image.
    download,
    /// Verify the freshly-programmed bank.
    verify,
    /// Latch the inactive bank and reboot.
    commit,
    /// Terminal or sentinel state: answer ok so the caller may stop polling.
    settle,
    /// A transient state with no cached manifest: invalid_state.
    refuse,
};

/// `internal_step_dispatch`'s switch, decided without touching module state.
pub fn stepAction(s: u8, manifest_valid: bool) StepAction {
    return switch (s) {
        state.idle => if (manifest_valid) .download else .check,
        // checking and downloading are always resolved synchronously inside a
        // single API call, so the host build never dispatches from them.
        state.checking, state.downloading => if (manifest_valid) .download else .refuse,
        state.verifying => .verify,
        state.committing => .commit,
        else => .settle,
    };
}
