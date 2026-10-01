//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RAM-backed media-download sink: the transaction state machine over a
//! buffer the caller owns.
//!
//! The adapter never allocates. `bind` takes the caller's bytes as a slice
//! and the transaction is bookkeeping over it: one private extent grows under
//! `write`, becomes readable only on `commit`, and is discarded by `abort`.
//! Bytes are not scrubbed on abort; the extent is what publication means.

/// Every way a transition can be refused, flattened to `ra8_err_t` at the
/// ABI membrane and nowhere else.
pub const Error = error{
    /// The coordinator's destination label was empty.
    InvalidArg,
    /// The transition does not exist from the current lifecycle state.
    InvalidState,
    /// The complete fragment does not fit in the remaining capacity.
    NoMem,
};

/// `ra8_mdl_storage_ram_t`. Declared in `inc/ra8_mdl_storage_ram.h`, which
/// the host suite reads fields back from directly, so the layout is part of
/// the contract rather than private to this file.
pub const Ram = extern struct {
    /// Caller-owned backing bytes.
    data: ?[*]u8 = null,
    /// Writable backing capacity.
    capacity: usize = 0,
    /// Private or committed byte count.
    length: usize = 0,
    /// A transfer may append bytes.
    active: bool = false,
    /// A read-only view may be exposed.
    committed: bool = false,

    /// Bind a fresh idle adapter over the caller's buffer.
    pub fn bind(buffer: []u8) Ram {
        return .{ .data = buffer.ptr, .capacity = buffer.len };
    }

    /// Start a fresh private object, clearing any prior committed extent.
    pub fn begin(self: *Ram, destination: []const u8) Error!void {
        if (destination.len == 0) return Error.InvalidArg;
        if (self.active) return Error.InvalidState;
        self.length = 0;
        self.committed = false;
        self.active = true;
    }

    /// Append one complete fragment, all of it or none of it.
    pub fn write(self: *Ram, bytes: []const u8) Error!void {
        if (!self.active) return Error.InvalidState;
        if (bytes.len > self.capacity - self.length) return Error.NoMem;
        if (bytes.len != 0) {
            @memcpy(self.backing()[self.length..][0..bytes.len], bytes);
        }
        self.length += bytes.len;
    }

    /// Publish the private extent. A state transition only: the bytes already
    /// sit in the caller's memory and no pointer escapes until `view`.
    pub fn commit(self: *Ram) Error!void {
        if (!self.active or self.length == 0) return Error.InvalidState;
        self.active = false;
        self.committed = true;
    }

    /// Discard the logical extent of the active private object.
    pub fn abort(self: *Ram) Error!void {
        if (!self.active) return Error.InvalidState;
        self.length = 0;
        self.active = false;
        self.committed = false;
    }

    /// The committed bytes, readable only between `commit` and the next
    /// `begin`.
    pub fn view(self: *const Ram) Error![]const u8 {
        if (!self.committed or self.active or self.length == 0) {
            return Error.InvalidState;
        }
        return self.data.?[0..self.length];
    }

    fn backing(self: *Ram) []u8 {
        return self.data.?[0..self.capacity];
    }
};
