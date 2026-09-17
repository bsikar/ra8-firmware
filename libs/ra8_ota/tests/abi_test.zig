//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the C ABI membrane of the ra8_ota parsing cluster: the
//! guard order inside `priv_ota_validate_cfg`, the exact log line each refusal
//! emits, and the end-to-end manifest decode.

const std = @import("std");
const abi = @import("abi");
const impl = abi.internal;

var log_calls: usize = 0;
var last_tag: [64]u8 = undefined;
var last_tag_len: usize = 0;
var last_message: [96]u8 = undefined;
var last_message_len: usize = 0;

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

fn resetLog() void {
    log_calls = 0;
    last_tag_len = 0;
    last_message_len = 0;
}

fn loggedTag() []const u8 {
    return last_tag[0..last_tag_len];
}

fn loggedMessage() []const u8 {
    return last_message[0..last_message_len];
}

const ok: u16 = 0;
const invalid_arg: u16 = 0x103;
const invalid_size: u16 = 0x105;
const null_ptr: u16 = 0x504;

// Stand-in callbacks: only their addresses matter to the validator.
fn netOpen(_: ?*anyopaque, _: [*:0]const u8, _: *u32) callconv(.c) u16 {
    return ok;
}
fn netRead(_: ?*anyopaque, _: [*]u8, _: u32, _: *u32) callconv(.c) u16 {
    return ok;
}
fn netClose(_: ?*anyopaque) callconv(.c) u16 {
    return ok;
}
fn shaInit(_: ?*anyopaque) callconv(.c) u16 {
    return ok;
}
fn shaUpdate(_: ?*anyopaque, _: [*]const u8, _: u32) callconv(.c) u16 {
    return ok;
}
fn shaFinal(_: ?*anyopaque, _: [*]u8) callconv(.c) u16 {
    return ok;
}
fn ecdsaVerify(_: ?*anyopaque, _: u32, _: [*]const u8, _: [*]const u8, _: u32) callconv(.c) u16 {
    return ok;
}
fn flashErase(_: ?*anyopaque, _: u32, _: u32) callconv(.c) u16 {
    return ok;
}
fn flashProgram(_: ?*anyopaque, _: u32, _: [*]const u8, _: u32) callconv(.c) u16 {
    return ok;
}
fn flashSetStartup(_: ?*anyopaque, _: u8, _: bool) callconv(.c) u16 {
    return ok;
}
fn flashReadback(_: ?*anyopaque, _: u32, _: [*]u8, _: u32) callconv(.c) u16 {
    return ok;
}

fn goodCfg() impl.Cfg {
    var cfg = std.mem.zeroes(impl.Cfg);
    cfg.net.open = netOpen;
    cfg.net.read = netRead;
    cfg.net.close = netClose;
    cfg.crypto.sha256_init = shaInit;
    cfg.crypto.sha256_update = shaUpdate;
    cfg.crypto.sha256_final = shaFinal;
    cfg.crypto.ecdsa_verify = ecdsaVerify;
    cfg.flash.erase = flashErase;
    cfg.flash.program = flashProgram;
    cfg.flash.set_startup = flashSetStartup;
    cfg.flash.readback = flashReadback;
    cfg.flash.bank_size_bytes = 0x1000;
    const url = "https://example.invalid/manifest.json";
    @memcpy(cfg.manifest_url[0..url.len], url);
    return cfg;
}

test "a fully wired configuration validates with no log line" {
    resetLog();
    var cfg = goodCfg();
    try std.testing.expectEqual(ok, abi.priv_ota_validate_cfg(&cfg));
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "a null configuration is refused first, under the ra8_ota tag" {
    resetLog();
    try std.testing.expectEqual(null_ptr, abi.priv_ota_validate_cfg(null));
    try std.testing.expectEqual(@as(usize, 1), log_calls);
    try std.testing.expectEqualStrings("ra8_ota", loggedTag());
    try std.testing.expectEqualStrings("cfg", loggedMessage());
}

test "the net block is judged before crypto and flash, in declaration order" {
    const cases = [_]struct { clear: []const u8, message: []const u8 }{
        .{ .clear = "open", .message = "net.open" },
        .{ .clear = "read", .message = "net.read" },
        .{ .clear = "close", .message = "net.close" },
    };
    inline for (cases) |case| {
        resetLog();
        var cfg = goodCfg();
        // Break one net pointer and also a later one, to prove the order.
        @field(cfg.net, case.clear) = null;
        cfg.crypto.sha256_init = null;
        cfg.flash.erase = null;
        try std.testing.expectEqual(null_ptr, abi.priv_ota_validate_cfg(&cfg));
        try std.testing.expectEqualStrings(case.message, loggedMessage());
    }
}

test "the crypto block is judged before flash, in declaration order" {
    const cases = [_]struct { clear: []const u8, message: []const u8 }{
        .{ .clear = "sha256_init", .message = "crypto.sha256_init" },
        .{ .clear = "sha256_update", .message = "crypto.sha256_update" },
        .{ .clear = "sha256_final", .message = "crypto.sha256_final" },
        .{ .clear = "ecdsa_verify", .message = "crypto.ecdsa_verify" },
    };
    inline for (cases) |case| {
        resetLog();
        var cfg = goodCfg();
        @field(cfg.crypto, case.clear) = null;
        cfg.flash.erase = null;
        try std.testing.expectEqual(null_ptr, abi.priv_ota_validate_cfg(&cfg));
        try std.testing.expectEqualStrings(case.message, loggedMessage());
    }
}

test "every flash callback is judged in declaration order" {
    const cases = [_]struct { clear: []const u8, message: []const u8 }{
        .{ .clear = "erase", .message = "flash.erase" },
        .{ .clear = "program", .message = "flash.program" },
        .{ .clear = "set_startup", .message = "flash.set_startup" },
        .{ .clear = "readback", .message = "flash.readback" },
    };
    inline for (cases) |case| {
        resetLog();
        var cfg = goodCfg();
        @field(cfg.flash, case.clear) = null;
        try std.testing.expectEqual(null_ptr, abi.priv_ota_validate_cfg(&cfg));
        try std.testing.expectEqualStrings(case.message, loggedMessage());
    }
}

test "the bank size gates answer invalid_arg and emit no log line" {
    resetLog();
    var zero = goodCfg();
    zero.flash.bank_size_bytes = 0;
    try std.testing.expectEqual(invalid_arg, abi.priv_ota_validate_cfg(&zero));
    try std.testing.expectEqual(@as(usize, 0), log_calls);

    resetLog();
    var over = goodCfg();
    over.flash.bank_size_bytes = impl.max_image_bytes + 1;
    try std.testing.expectEqual(invalid_arg, abi.priv_ota_validate_cfg(&over));
    try std.testing.expectEqual(@as(usize, 0), log_calls);

    var edge = goodCfg();
    edge.flash.bank_size_bytes = impl.max_image_bytes;
    try std.testing.expectEqual(ok, abi.priv_ota_validate_cfg(&edge));
}

test "an empty manifest URL is the last gate and answers invalid_arg silently" {
    resetLog();
    var cfg = goodCfg();
    cfg.manifest_url[0] = 0;
    try std.testing.expectEqual(invalid_arg, abi.priv_ota_validate_cfg(&cfg));
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "the exported MC/DC predicates carry the C semantics across the membrane" {
    try std.testing.expect(abi.priv_ota_char_in_range('c', 'a', 'f'));
    try std.testing.expect(!abi.priv_ota_char_in_range('g', 'a', 'f'));
    try std.testing.expect(!abi.priv_ota_char_in_range('A', 'a', 'f'));

    try std.testing.expect(!abi.priv_ota_download_state_invalid(0, 1, 0));
    try std.testing.expect(!abi.priv_ota_download_state_invalid(0, 1, 1));
    try std.testing.expect(abi.priv_ota_download_state_invalid(0, 1, 2));
}

test "priv_ota_json_u32 is reachable through the membrane" {
    var v: u32 = 0;
    try std.testing.expectEqual(ok, abi.priv_ota_json_u32("{\"size\": 65536}", "\"size\"", &v));
    try std.testing.expectEqual(@as(u32, 65536), v);
    try std.testing.expectEqual(
        invalid_arg,
        abi.priv_ota_json_u32("{\"size\": 65536}", "\"nope\"", &v),
    );
}

fn hexOf(comptime byte: u8, comptime count: usize) [count * 2]u8 {
    var out: [count * 2]u8 = undefined;
    const digits = "0123456789abcdef";
    var i: usize = 0;
    while (i < count) : (i += 1) {
        out[i * 2] = digits[byte >> 4];
        out[(i * 2) + 1] = digits[byte & 0x0F];
    }
    return out;
}

const digest_hex = hexOf(0xAB, 32);
const signature_hex = hexOf(0x5C, 70);

fn manifestJson(comptime size_text: []const u8) [:0]const u8 {
    return "{\"version\": \"2.0.1\", \"url\": \"https://example.invalid/fw.bin\", " ++
        "\"size\": " ++ size_text ++ ", \"sha256\": \"" ++ &digest_hex ++
        "\", \"signature\": \"" ++ &signature_hex ++ "\"}";
}

test "a complete manifest decodes every field" {
    var m = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(ok, abi.priv_ota_manifest_decode(manifestJson("4096"), &m));
    try std.testing.expectEqualStrings("2.0.1", std.mem.sliceTo(m.version[0..], 0));
    try std.testing.expectEqualStrings(
        "https://example.invalid/fw.bin",
        std.mem.sliceTo(m.image_url[0..], 0),
    );
    try std.testing.expectEqual(@as(u32, 4096), m.image_size_bytes);
    try std.testing.expectEqual(@as(u16, 70), m.signature_len);
    for (m.image_sha256) |b| try std.testing.expectEqual(@as(u8, 0xAB), b);
    for (m.signature[0..70]) |b| try std.testing.expectEqual(@as(u8, 0x5C), b);
    // The tail past signature_len stays zeroed by the leading memset.
    for (m.signature[70..]) |b| try std.testing.expectEqual(@as(u8, 0), b);
}

test "the manifest size gates keep their two different codes" {
    var m = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(invalid_arg, abi.priv_ota_manifest_decode(manifestJson("0"), &m));

    var over = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(
        invalid_size,
        abi.priv_ota_manifest_decode(manifestJson("524289"), &over),
    );

    var edge = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(ok, abi.priv_ota_manifest_decode(manifestJson("524288"), &edge));
}

test "a manifest decode zeroes the destination before it starts" {
    var m = std.mem.zeroes(impl.Manifest);
    m.signature_len = 0x7777;
    @memset(m.version[0..], 'Z');
    // No "version" key at all: the decode fails on the first field.
    try std.testing.expectEqual(
        invalid_arg,
        abi.priv_ota_manifest_decode("{\"url\": \"x\"}", &m),
    );
    try std.testing.expectEqual(@as(u16, 0), m.signature_len);
    for (m.version) |b| try std.testing.expectEqual(@as(u8, 0), b);
}

test "a wrong-length digest and an undecodable signature are both invalid_arg" {
    const short_digest = "{\"version\": \"1\", \"url\": \"u\", \"size\": 8, " ++
        "\"sha256\": \"abcd\", \"signature\": \"" ++ &signature_hex ++ "\"}";
    var m = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(invalid_arg, abi.priv_ota_manifest_decode(short_digest, &m));

    const odd_signature = "{\"version\": \"1\", \"url\": \"u\", \"size\": 8, " ++
        "\"sha256\": \"" ++ &digest_hex ++ "\", \"signature\": \"abc\"}";
    var n = std.mem.zeroes(impl.Manifest);
    try std.testing.expectEqual(invalid_arg, abi.priv_ota_manifest_decode(odd_signature, &n));
}
