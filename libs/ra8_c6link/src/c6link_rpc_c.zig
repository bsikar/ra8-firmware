//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The private `ra8_c6link_internal.h` view, which brings in the vendored
//! esp-hosted RPC codec types (`Rpc` and its request and response bodies)
//! the Wi-Fi record types from `ra8_c6link_wifi.h`, and the C ABI the
//! remaining C exports. `RA8_FREESTANDING` makes the
//! protobuf-c fork route `assert` through `ra8_check.h` instead of
//! `<assert.h>`, so this translates without libc on host and on Arm alike.

pub const c = @cImport({
    @cDefine("RA8_FREESTANDING", "1");
    @cDefine("static_assert", "_Static_assert");
    @cDefine("alignas", "_Alignas");
    @cInclude("stdbool.h");
    @cInclude("ra8_c6link_internal.h");
    @cInclude("ra8_c6link_wifi.h");
});
