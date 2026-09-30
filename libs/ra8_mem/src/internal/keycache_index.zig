//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! How `ra8_keycache` finds a cell from a key: the hash policy and the bucket
//! chains threaded through the cell metadata. Keys are opaque blobs compared
//! byte-wise, stored out of line in a caller-owned array.
//!
//! The hash is injectable (a facade whose key carries structure supplies its
//! own); folding the raw 32-bit value into `[0, bucket_count)` is always this
//! file's, so a callback never has to know the bucket count.
//!
//! No `extern` and no engine: this file is a host test root on its own.

const std = @import("std");

const list = @import("keycache_list.zig");

pub const Cell = list.Cell;

/// FNV-1a 32-bit parameters, as `pub const` under a namespace rather than the
/// C-style `k_` enum constants the C used.
pub const fnv = struct {
    pub const offset_basis: u32 = 2166136261;
    pub const prime: u32 = 16777619;
};

/// `ra8_keycache_hash_fn`: hash a key blob to a raw 32-bit value.
pub const HashFn = *const fn (key: ?*const anyopaque, key_bytes: u32, ctx: ?*anyopaque) callconv(.c) u32;

/// The built-in hash: FNV-1a over the key bytes. Selected when the config
/// leaves `hash` null.
pub fn fnv1a(key: []const u8) u32 {
    var h: u32 = fnv.offset_basis;
    for (key) |b| {
        h ^= b;
        h *%= fnv.prime;
    }
    return h;
}

/// The key-to-cell map: the buckets, the chains threaded through the cell
/// metadata, and the key storage they are keyed on. A view over caller-owned
/// memory, built fresh from the config at each call rather than stored.
pub const Table = struct {
    /// `bucket_count` chain heads, each a cell index or `list.none`.
    buckets: []i32,
    /// One metadata record per cell; `hash_next` is this file's field.
    meta: []Cell,
    /// `cell_count * key_bytes` of key storage.
    keys: []const u8,
    /// Bytes per key.
    key_bytes: u32,
    /// Injected hash, or null for the built-in FNV-1a.
    hash: ?HashFn,
    /// Opaque context handed to `hash`.
    hash_ctx: ?*anyopaque,

    /// Cell `idx`'s stored key.
    pub fn keyOf(self: Table, idx: u32) []const u8 {
        const start = idx * self.key_bytes;
        return self.keys[start..][0..self.key_bytes];
    }

    /// The bucket `key` belongs in.
    pub fn bucketOf(self: Table, key: []const u8) u32 {
        const raw = if (self.hash) |h| h(key.ptr, self.key_bytes, self.hash_ctx) else fnv1a(key);
        return raw % @as(u32, @intCast(self.buckets.len));
    }

    /// Prepend cell `f` to the chain of the bucket its stored key hashes to.
    pub fn insert(self: Table, f: u32) void {
        const b = self.bucketOf(self.keyOf(f));
        self.meta[f].hash_next = self.buckets[b];
        self.buckets[b] = @intCast(f);
    }

    /// Unthread cell `f` from its chain. Bounded by the cell count, so a
    /// corrupt chain terminates instead of spinning (NASA P10 Rule 2).
    pub fn remove(self: Table, f: u32) void {
        const b = self.bucketOf(self.keyOf(f));
        const target: i32 = @intCast(f);
        if (self.buckets[b] == target) {
            self.buckets[b] = self.meta[f].hash_next;
        } else {
            var cur = self.buckets[b];
            var guard: usize = 0;
            while (cur != list.none and guard < self.meta.len) : (guard += 1) {
                const idx: u32 = @intCast(cur);
                if (self.meta[idx].hash_next == target) {
                    self.meta[idx].hash_next = self.meta[f].hash_next;
                    break;
                }
                cur = self.meta[idx].hash_next;
            }
        }
        self.meta[f].hash_next = list.none;
    }

    /// The valid cell holding `key`, or null when it is not resident.
    pub fn lookup(self: Table, key: []const u8) ?u32 {
        var cur = self.buckets[self.bucketOf(key)];
        var guard: usize = 0;
        while (cur != list.none and guard < self.meta.len) : (guard += 1) {
            const idx: u32 = @intCast(cur);
            if (self.meta[idx].valid != 0 and std.mem.eql(u8, self.keyOf(idx), key)) return idx;
            cur = self.meta[idx].hash_next;
        }
        return null;
    }

    /// Clear every bucket. The chains themselves live in the cell metadata,
    /// which the recency seed resets.
    pub fn clear(self: Table) void {
        @memset(self.buckets, list.none);
    }
};
