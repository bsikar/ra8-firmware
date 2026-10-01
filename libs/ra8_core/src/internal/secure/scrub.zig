//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Erasure of secret material that the optimiser may not delete.
//!
//! A plain `@memset` just before a buffer leaves scope is a dead store: the
//! compiler is free to drop it, and the key, MAC or digest stays in stack or
//! static memory for whatever runs next. Writing through a volatile pointer
//! makes each store an observable side effect the optimiser has to keep, so
//! the scrub actually happens. This erases; it does not compare, and it is
//! not itself a constant-time operation.

/// Overwrite every byte with zero through volatile stores. An empty slice
/// writes nothing.
pub fn zeroize(bytes: []u8) void {
    const dst: [*]volatile u8 = @ptrCast(bytes.ptr);
    for (0..bytes.len) |i| {
        dst[i] = 0;
    }
}
