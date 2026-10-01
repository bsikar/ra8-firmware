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
pub const mdl_pull = @import("mdl_pull.zig");
pub const mdl_envelope = @import("mdl_envelope.zig");
pub const mdl_issue = @import("mdl_issue.zig");
pub const mdl_decode = @import("mdl_decode.zig");
pub const mdl_chunk_decode = @import("mdl_chunk_decode.zig");
pub const mdl_encode = @import("mdl_encode.zig");
pub const mdl_wire = @import("mdl_wire.zig");
pub const mdl_wire_read = @import("mdl_wire_read.zig");
pub const mdl_take = @import("mdl_take.zig");
pub const mdl_request_decode = @import("mdl_request_decode.zig");
pub const mdl_reply_encode = @import("mdl_reply_encode.zig");
pub const mdl_service_cancel = @import("mdl_service_cancel.zig");
pub const mdl_chunk_bound = @import("mdl_chunk_bound.zig");
pub const mdl_service_next = @import("mdl_service_next.zig");
pub const mdl_start_text = @import("mdl_start_text.zig");
pub const mdl_service_start = @import("mdl_service_start.zig");
pub const rpc_wait = @import("rpc_wait.zig");
pub const sta_cfg = @import("sta_cfg.zig");
pub const rx_route = @import("rx_route.zig");
pub const field_copy = @import("field_copy.zig");
pub const tx_admit = @import("tx_admit.zig");
pub const wifi_init = @import("wifi_init.zig");
pub const bare_rpc = @import("bare_rpc.zig");
pub const sta_policy = @import("sta_policy.zig");
