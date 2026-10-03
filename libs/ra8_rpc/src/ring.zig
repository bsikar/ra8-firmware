//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! One single-producer, single-consumer byte ring in memory two cores share.
//!
//! The producer owns `head` and the consumer owns `tail`; each writes only
//! its own. An index counts bytes from the start of the data and is always
//! less than the capacity. The ring is empty when the two are equal, so one
//! byte is always left unused and a ring holds `capacity - 1` bytes.
//!
//! This file knows the layout and nothing about ordering. When each store is
//! made, and what stands between them, belongs to `ring_transport.zig`.

const std = @import("std");

/// Where everything is. On the wire between two cores, so pinned.
pub const Layout = struct {
    /// The bytes `RA8B` at the start of every ring.
    pub const magic: u32 = 0x42384152;
    pub const version: u32 = 1;

    /// One cache line. `head` and `tail` each get a line to themselves, so
    /// the two cores never write the same one.
    pub const line = 32;

    /// Byte offsets of the header's fields. Each is a little-endian `u32`.
    pub const At = struct {
        pub const magic = 0;
        pub const version = 4;
        pub const capacity = 8;
        pub const head = line;
        pub const tail = 2 * line;
    };

    /// The data starts here, three lines in.
    pub const header_bytes = 3 * line;
    /// The smallest ring that can hold a byte.
    pub const min_capacity = 2;
};

/// A snapshot of the two indices, already checked to be in range.
pub const Indices = struct { head: u32, tail: u32 };

pub const Ring = struct {
    /// Header and data. Caller-owned, shared with the other core.
    mem: []align(4) u8,

    pub const InitError = error{
        /// No room for the header and two bytes of data.
        RingTooSmall,
        /// More data than a `u32` index can address.
        RingTooBig,
    };

    /// Use `mem` as a ring. Nothing is written; see `writeHeader`.
    pub fn init(mem: []align(4) u8) InitError!Ring {
        if (mem.len < Layout.header_bytes + Layout.min_capacity) return error.RingTooSmall;
        if (mem.len - Layout.header_bytes > std.math.maxInt(u32)) return error.RingTooBig;
        return .{ .mem = mem };
    }

    /// Write a fresh header: an empty ring of this memory's capacity. One
    /// core does this, with the other stopped, before either uses the ring.
    pub fn writeHeader(self: Ring) void {
        @memset(self.mem[0..Layout.header_bytes], 0);
        self.word(Layout.At.capacity).* = std.mem.nativeToLittle(u32, self.capacity());
        self.word(Layout.At.version).* = std.mem.nativeToLittle(u32, Layout.version);
        self.word(Layout.At.magic).* = std.mem.nativeToLittle(u32, Layout.magic);
    }

    /// Data bytes in the ring. One of them is never used.
    pub fn capacity(self: Ring) u32 {
        return @intCast(self.mem.len - Layout.header_bytes);
    }

    /// Read the header. The other core wrote part of it, so none of it is
    /// believed until it has been checked: a wrong magic, version or
    /// capacity, or an index past the data, is `BadMessage`.
    pub fn load(self: Ring) error{BadMessage}!Indices {
        const indices: Indices = .{
            .head = self.read(Layout.At.head),
            .tail = self.read(Layout.At.tail),
        };
        if (self.read(Layout.At.magic) != Layout.magic) return error.BadMessage;
        if (self.read(Layout.At.version) != Layout.version) return error.BadMessage;
        if (self.read(Layout.At.capacity) != self.capacity()) return error.BadMessage;
        if (indices.head >= self.capacity()) return error.BadMessage;
        if (indices.tail >= self.capacity()) return error.BadMessage;
        return indices;
    }

    /// Bytes written and not yet read.
    pub fn used(self: Ring, indices: Indices) u32 {
        if (indices.head >= indices.tail) return indices.head - indices.tail;
        return self.capacity() - indices.tail + indices.head;
    }

    /// Bytes that can be written before the ring is full.
    pub fn free(self: Ring, indices: Indices) u32 {
        return self.capacity() - 1 - self.used(indices);
    }

    /// The index `count` bytes after `index`, for a `count` below capacity.
    pub fn advance(self: Ring, index: u32, count: u32) u32 {
        const to_end = self.capacity() - index;
        return if (count >= to_end) count - to_end else index + count;
    }

    /// Copy `bytes` into the data starting at `index`, wrapping at the end.
    pub fn write(self: Ring, index: u32, bytes: []const u8) void {
        const data = self.mem[Layout.header_bytes..];
        const first = @min(bytes.len, data.len - index);
        @memcpy(data[index..][0..first], bytes[0..first]);
        @memcpy(data[0 .. bytes.len - first], bytes[first..]);
    }

    /// Copy data starting at `index` into `into`, wrapping at the end.
    pub fn copy(self: Ring, index: u32, into: []u8) void {
        const data = self.mem[Layout.header_bytes..];
        const first = @min(into.len, data.len - index);
        @memcpy(into[0..first], data[index..][0..first]);
        @memcpy(into[first..], data[0 .. into.len - first]);
    }

    pub fn publishHead(self: Ring, head: u32) void {
        self.word(Layout.At.head).* = std.mem.nativeToLittle(u32, head);
    }

    pub fn publishTail(self: Ring, tail: u32) void {
        self.word(Layout.At.tail).* = std.mem.nativeToLittle(u32, tail);
    }

    fn read(self: Ring, offset: usize) u32 {
        return std.mem.littleToNative(u32, self.word(offset).*);
    }

    /// One header field as a single aligned word: the other core must never
    /// see half of an index, so it is loaded and stored whole.
    fn word(self: Ring, offset: usize) *volatile u32 {
        return @ptrCast(@alignCast(self.mem[offset..][0..@sizeOf(u32)]));
    }
};
