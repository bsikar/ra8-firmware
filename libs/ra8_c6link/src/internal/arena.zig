//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The fixed decode arena that lets the protobuf codec run with no heap.
//!
//! `rpc__unpack()` allocates one block per message, nested message, repeated
//! field and binary field. This firmware has no heap, so the codec draws from a
//! bump allocator over a caller-supplied buffer instead. Every block a decode
//! takes is released by the matching `rpc__free_unpacked()` and the RPC layer
//! empties the arena straight after, so the peak is one message, not one run.
//!
//! `free` is not a no-op: protobuf-c unwinds a failed decode newest-first, so
//! freeing the newest block rolls the bump offset back and reclaims exactly the
//! space that decode would otherwise strand.

/// Alignment of every block the arena hands out. protobuf-c stores `uint64_t`
/// fields in these blocks, so eight bytes is a correctness requirement.
pub const alignment: u32 = 8;

/// Rounding mask for `alignment`.
pub const mask: u32 = alignment - 1;

comptime {
    if (alignment & mask != 0) @compileError("arena alignment must be a power of two");
}

/// The four `ra8_c6link_t` fields the arena owns, in their C order.
///
/// `last` is one past the offset of the newest block, or zero when no block can
/// be rolled back. `used` never exceeds `bytes`.
pub const Arena = extern struct {
    base: ?[*]u8,
    bytes: u32,
    used: u32,
    last: u32,

    /// Take a `size`-byte block, or null when the arena cannot serve it.
    pub fn alloc(self: *Arena, size: usize) ?[*]u8 {
        const base = self.base orelse return null;
        // `used <= bytes`, so this cannot underflow, and every later
        // comparison stays inside the remaining space.
        const avail = self.bytes - self.used;
        if (size > avail) return null;
        const want: u32 = @intCast(size);
        const pad = (0 -% want) & mask;
        if (pad > avail - want) return null;

        const at = self.used;
        self.used = at + want + pad;
        self.last = at + 1;
        return base + at;
    }

    /// Return a block. Only the newest block rolls the offset back; any other
    /// is kept until `reset`.
    pub fn free(self: *Arena, pointer: ?*const anyopaque) void {
        const ptr = pointer orelse return;
        if (self.last == 0) return;
        const base = self.base orelse return;
        const at = self.last - 1;
        if (@intFromPtr(ptr) == @intFromPtr(base + at)) {
            self.used = at;
            self.last = 0;
        }
    }

    /// Empty the arena. The bytes are left as they are, not scrubbed.
    pub fn reset(self: *Arena) void {
        self.used = 0;
        self.last = 0;
    }
};
