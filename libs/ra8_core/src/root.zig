//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_core`. One seam of this library is Zig so far: the
//! freestanding runtime primitives (#2820). Referencing the membrane is
//! what pulls its exports into the archive.

comptime {
    _ = @import("freestanding_abi");
}
