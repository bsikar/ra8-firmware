//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The library's internal modules, gathered for the unit tests.
//!
//! Nothing here crosses the C ABI; the membrane is
//! `src/ra8_tz_secure_boot_abi.zig`, which drives these decisions against
//! real silicon.

pub const regs = @import("regs.zig");
pub const boot = @import("boot.zig");
pub const ipc = @import("ipc.zig");
pub const partition = @import("partition.zig");
pub const sau = @import("sau.zig");
pub const nsimage = @import("nsimage.zig");
pub const psar = @import("psar.zig");

test {
    @import("std").testing.refAllDecls(@This());
}
