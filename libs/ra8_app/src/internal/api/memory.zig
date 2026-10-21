//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Whether a pointer a module handed over names bytes the module owns. This
//! is ThreadX's `TXM_MODULE_MANAGER_CHECK_INSIDE_DATA` rule restated: the
//! whole range, not only its first byte, lies in the module's data region or
//! in a shared region the manager gave it, and the range does not wrap.

const std = @import("std");
const abi = @import("abi.zig");

/// A half-open address range, `[start, end)`.
pub const Region = struct {
    start: abi.Word = 0,
    end: abi.Word = 0,

    fn holds(self: Region, address: abi.Word, len: abi.Word) bool {
        if (address < self.start) return false;
        const last = std.math.add(abi.Word, address, len) catch return false;
        return last <= self.end;
    }
};

/// What the module may name: its data region (`txm_module_instance_data_start`
/// to `_data_end` + 1), and the shared region if it has one.
pub const Owned = struct {
    data: Region,
    shared: ?Region = null,

    /// Whether `[address, address + len)` is all the module's. A zero-length
    /// range still needs an address inside a region, and a null pointer is
    /// never the module's.
    pub fn holds(self: Owned, address: abi.Word, len: abi.Word) bool {
        if (address == 0) return false;
        if (self.data.holds(address, len)) return true;
        if (self.shared) |shared| return shared.holds(address, len);
        return false;
    }

    /// Whether `[address, address + len)` is the module's and aligned for `T`.
    pub fn holdsFor(self: Owned, comptime T: type, address: abi.Word) bool {
        if (address % @alignOf(T) != 0) return false;
        return self.holds(address, @sizeOf(T));
    }
};
