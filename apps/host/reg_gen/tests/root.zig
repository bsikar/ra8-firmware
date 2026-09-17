//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! Single build-graph root for the dedicated reg_gen test modules.

comptime {
    _ = @import("c23_cc.zig");
    _ = @import("c23_cc_test.zig");
    _ = @import("generator_test.zig");
    _ = @import("main_test.zig");
}
