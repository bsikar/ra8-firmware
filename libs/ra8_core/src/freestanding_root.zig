//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the FREESTANDING archive: the libc subset the firmware provides
//! for itself, and nothing else.
//!
//! It is its own archive because its exported names are the bare standard
//! ones (`memcpy`, `memset`, `strlen`, `abs`) that an image needs and that a
//! host test binary already has a real libc for. Everything else ra8_core
//! ports goes in `root.zig` / the `ra8_core_zig` archive, which a host test
//! can link unconditionally.

comptime {
    _ = @import("freestanding_abi");
}
