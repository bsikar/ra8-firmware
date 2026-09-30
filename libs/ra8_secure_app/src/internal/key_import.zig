//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Secure-side sealed-key import: the wrapped-blob format and the slot
//! allocator behind it.
//!
//! A sealed blob is 32 key bytes followed by a 16-byte AES-CMAC over exactly
//! those key bytes, keyed by the vault's key-authentication key (KAK). Import
//! authenticates the blob, takes the lowest free slot, stores the key, and
//! hands back an opaque handle. The slot index never crosses the veneer.
//!
//! The KAK lives in its own vault store, separate from the NS-importable slot
//! array, so nothing the Non-Secure world can reach ever keys the MAC. Every
//! path that touches it wipes its copy before returning, so no key material is
//! left on the secure stack.
//!
//! Fail-closed behaviour is inherited rather than re-stated: every path here
//! reaches the KAK through `vault.loadMacKey`, which answers `not_supported`
//! when the vault body is not in this image, so an image without a hardened
//! backend cannot import a key at all.

const std = @import("std");

const cmac = @import("cmac.zig");
const key_handle = @import("key_handle.zig");
const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// The handle space. Re-exported so a consumer of the import path reaches the
/// handle vocabulary through the module it is already importing.
pub const handle = key_handle;

/// The key store this path imports into. Public for the same reason: whoever
/// provisions the KAK is already holding this module.
pub const vault = @import("vault.zig");

/// The blob layout, mirroring `src/key_import_internal.h`.
pub const Blob = struct {
    /// 32 key bytes plus a 16-byte tag.
    pub const bytes: u16 = 48;
    /// Trailing AES-CMAC tag length.
    pub const mac_bytes: u16 = 16;
    /// Key portion length.
    pub const key_bytes: u16 = 32;
};

/// One sealed blob.
pub const Sealed = [Blob.bytes]u8;

comptime {
    std.debug.assert(Blob.key_bytes + Blob.mac_bytes == Blob.bytes);
    // The key portion is what the vault stores, so the two must agree.
    std.debug.assert(Blob.key_bytes == vault.Limits.key_bytes);
    // One bit per slot, in the bitmap below.
    std.debug.assert(vault.Limits.slots <= @bitSizeOf(u16));
}

/// Bit `i` is set when slot `i` holds an imported key.
var live: u16 = 0;

/// A borrowed copy of the vault KAK, wiped by `deinit`.
///
/// This exists so the wipe is a `defer` at each call site rather than a step
/// three functions have to remember on every return path, error paths included.
const MacKey = struct {
    buf: [vault.Limits.mac_key_bytes]u8 = undefined,
    len: u16 = 0,

    fn load(self: *MacKey) Err {
        return vault.loadMacKey(&self.buf, &self.len);
    }

    fn bytes(self: *const MacKey) []const u8 {
        return self.buf[0..self.len];
    }

    fn deinit(self: *MacKey) void {
        std.crypto.secureZero(u8, &self.buf);
        self.len = 0;
    }
};

/// The key portion of a sealed blob.
fn keyOf(blob: *const Sealed) *const [Blob.key_bytes]u8 {
    return blob[0..Blob.key_bytes];
}

/// The tag portion of a sealed blob.
fn tagOf(blob: *const Sealed) *const [Blob.mac_bytes]u8 {
    return blob[Blob.key_bytes..][0..Blob.mac_bytes];
}

fn bitFor(slot: u16) u16 {
    return @as(u16, 1) << @intCast(slot);
}

/// The lowest free slot, or null when every slot is in use.
fn lowestFree() ?u16 {
    var slot: u16 = 0;
    while (slot < vault.Limits.slots) : (slot += 1) {
        if (live & bitFor(slot) == 0) return slot;
    }
    return null;
}

/// Free every slot and reroll the handle salt.
///
/// Handles issued before this call stop resolving, both because their slots are
/// no longer live and because the salt moved.
pub fn reset() Err {
    live = 0;
    key_handle.reroll();
    return .ok;
}

/// Authenticate `blob` under the vault KAK.
///
/// A tampered blob and one sealed under a different KAK are both `invalid_arg`:
/// the verdict leaks nothing beyond go/no-go.
pub fn authenticate(blob: *const Sealed) Err {
    var kak: MacKey = .{};
    const loaded = kak.load();
    if (loaded != .ok) return loaded;
    defer kak.deinit();
    return cmac.verify(kak.bytes(), keyOf(blob), tagOf(blob));
}

/// Authenticate `blob`, store its key in the lowest free slot, and issue a
/// handle for it.
///
/// Nothing is mutated on a failing path: the tag is checked before a slot is
/// taken, and the slot is marked live only once the vault holds the key.
pub fn seal(blob: *const Sealed, out_handle: *u32) Err {
    const authentic = authenticate(blob);
    if (authentic != .ok) return authentic;

    const slot = lowestFree() orelse return .no_mem;

    const stored = vault.store(slot, keyOf(blob));
    if (stored != .ok) return stored;

    live |= bitFor(slot);
    out_handle.* = key_handle.forSlot(slot);
    return .ok;
}

/// Resolve a handle back to its slot.
///
/// Only a live slot can match, so a handle whose slot has since been reset does
/// not resolve even though the arithmetic that produced it is reproducible.
pub fn resolve(value: u32, out_slot: *u16) Err {
    var slot: u16 = 0;
    while (slot < vault.Limits.slots) : (slot += 1) {
        if (live & bitFor(slot) != 0 and key_handle.forSlot(slot) == value) {
            out_slot.* = slot;
            return .ok;
        }
    }
    return .not_found;
}

/// Seal `material` into `out_blob` under the vault KAK.
///
/// The provisioning and test side of the format: it produces exactly what
/// `seal` accepts, and it is the only way to build a blob without holding the
/// KAK yourself.
pub fn buildBlob(material: *const [Blob.key_bytes]u8, out_blob: *Sealed) Err {
    var kak: MacKey = .{};
    const loaded = kak.load();
    if (loaded != .ok) return loaded;
    defer kak.deinit();

    out_blob[0..Blob.key_bytes].* = material.*;
    return cmac.compute(
        kak.bytes(),
        material,
        out_blob[Blob.key_bytes..][0..Blob.mac_bytes],
    );
}
