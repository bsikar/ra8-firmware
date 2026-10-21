//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_app_api_t`, the calls a loaded module makes into the resident image
//! (RA8FW-294). `abi` is what both sides agree on, `module` is the module's
//! typed shims, and `kernel` is the resident image's checked dispatch.

pub const abi = @import("api/abi.zig");
pub const memory = @import("api/memory.zig");
pub const sandbox = @import("api/sandbox.zig");
pub const kernel = @import("api/kernel.zig");
pub const module = @import("api/module.zig");
pub const appimg = @import("appimg.zig");
