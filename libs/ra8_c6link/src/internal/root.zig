//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Module root for the Zig implementation of `ra8_c6link`, so the internal
//! tests reach the wire layers and their vocabulary by one import.

pub const vocab = @import("vocab.zig");
pub const tlv = @import("tlv.zig");
pub const frame = @import("frame.zig");
pub const caps = @import("caps.zig");
pub const storage_ram = @import("storage_ram.zig");
pub const mdl_types = @import("mdl_types.zig");
pub const mdl_transfer = @import("mdl_transfer.zig");
pub const mdl_request = @import("mdl_request.zig");
pub const mdl_chunk = @import("mdl_chunk.zig");
pub const mdl_service_rules = @import("mdl_service_rules.zig");
pub const mdl_session = @import("mdl_session.zig");
