//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one place that writes a NUL-terminated C string. The facade works in
//! slices; this is where a caller's fixed `char[]` gets filled.

/// Copy as much of `text` as fits into `dst`, always terminating. `dst` is a
/// valid C string on return even when `text` was longer and had to be cut.
pub fn write(dst: []u8, text: []const u8) void {
    if (dst.len == 0) return;
    const room = dst.len - 1;
    const taken = @min(room, text.len);
    @memcpy(dst[0..taken], text[0..taken]);
    dst[taken] = 0;
}
