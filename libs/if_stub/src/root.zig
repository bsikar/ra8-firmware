//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `if_stub`, the proof binding behind the neutral `fw_if_*`
//! ports. Referencing every membrane is what keeps its exports in the
//! archive.

comptime {
    _ = @import("clock_stub_abi");
    _ = @import("clock_stub_ops_abi");
    _ = @import("rng_stub_abi");
    _ = @import("gpio_stub_abi");
    _ = @import("uart_stub_abi");
}
