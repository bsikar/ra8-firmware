//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Composition root for the `libs/if` facade surface: the four peripheral
//! facades in one module, so a consumer that wants clock, rng, gpio and uart
//! in one compilation gets them without four claims on `internal/root.zig`.
//! The fs and path facades stay out on purpose: they carry the `fw_os`
//! translation the library root already wires, and no proof or image in this
//! tree needs them freestanding.
//!
//! The installed `if` archive is unchanged; this file is a module root for
//! consumers that compose the facades in Zig, not a new artifact.

pub const clock = @import("fw_if_clock_abi.zig");
pub const rng = @import("fw_if_rng_abi.zig");
pub const gpio = @import("fw_if_gpio_abi.zig");
pub const uart = @import("fw_if_uart_abi.zig");
