//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_ov5640`. The driver membrane and the house-I2C binder
//! live in separate files, so each needs an explicit comptime reference or its
//! exports never reach the static library.

comptime {
    _ = @import("ra8_ov5640_abi.zig");
    _ = @import("ra8_ov5640_bind_abi.zig");
}
