//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! Single build-graph root for the dedicated tools/zig_build test modules.

comptime {
    _ = @import("macos_host_test.zig");
}
