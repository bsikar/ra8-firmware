//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the SINGLE archive a freestanding image links: the libc subset of
//! `freestanding_root.zig` and the ordinary `ra8_*` ports of `root.zig`, in
//! one artifact named `ra8_core`.
//!
//! The split those two roots describe is a HOST concern. A host test binary
//! already has a real libc, so bare `memcpy` / `strlen` cannot ride in the
//! archive it links, and the two roots keep them apart. An image has no libc
//! at all: it needs the bare names AND the ports, and nothing it links
//! defines either of them twice.
//!
//! The split is also UNREACHABLE from an image, which is what this file is
//! really for. `_ra8_zig_build_archive()` in cmake/ra8_app/zig_libs.cmake
//! names a cross-built archive `lib<lib>.a`, so
//! `ra8_link_zig_library_for_cpu(LIB ra8_core)` can only ever fetch
//! `libra8_core.a`. Under the host split that is the freestanding half alone,
//! so an image asking ra8_core for anything else linked against a missing
//! symbol. Composing one archive here keeps that helper, and every existing
//! caller of it, unchanged.

comptime {
    _ = @import("freestanding_abi");
    _ = @import("pin_validator_abi");
    _ = @import("systick_abi");
    _ = @import("time_interface_systick_abi");
    _ = @import("time_abi");
    _ = @import("log_abi");
    _ = @import("decomp_abi");
    _ = @import("scb_abi");
    _ = @import("exception_abi");
    _ = @import("crashlog_abi");
    _ = @import("error_handler_abi");
    _ = @import("error_sink_abi");
    _ = @import("infrastructure_abi");
}
