//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Mirrors of the media-download C types the transfer coordinator is handed.
//!
//! Every struct here is declared in a public header the host suites build
//! their fixtures from (`ra8_c6link_mdl.h`, `ra8_c6link_mdl_transfer.h`,
//! `ra8_mdl_request.h`), so these layouts are contract rather than
//! implementation detail. They are plain data: no alignment attributes and no
//! padding-tuned field order.
//!
//! `ra8_c6link_t` is deliberately absent. The coordinator passes the link
//! handle straight back into the C media RPC and never reads a field of it,
//! so it travels as an opaque pointer.

/// `mdl_format_t`, `enum : uint8_t`.
pub const Format = struct {
    pub const loose: u8 = 0;
    pub const rabook: u8 = 8;
    pub const invalid: u8 = 255;
};

/// `ra8_mdl_state_t`, `enum : uint8_t`.
pub const State = struct {
    pub const accepted: u8 = 1;
    pub const downloading: u8 = 2;
    pub const complete: u8 = 3;
    pub const cancelled: u8 = 4;
    pub const failed: u8 = 5;
};

/// Bounds the coordinator enforces, from `ra8_mdl_protocol.h` and
/// `ra8_c6link_mdl_transfer.h`.
pub const Limit = struct {
    pub const sha256_bytes: usize = 32;
    pub const chunk_data_max: u16 = 1024;
    pub const transfer_bytes_max: u64 = 67108864;
    pub const retry_after_max: usize = 64;
    pub const etag_max: usize = 128;
    pub const http_date_max: usize = 64;
    pub const content_type_max: usize = 128;
};

/// `ra8_mdl_http_policy_t`.
pub const HttpPolicy = extern struct {
    user_agent: ?[*:0]const u8 = null,
    referer: ?[*:0]const u8 = null,
    if_none_match: ?[*:0]const u8 = null,
    if_modified_since: ?[*:0]const u8 = null,
    timeout_ms: u32 = 0,
};

/// `ra8_mdl_http_response_t`.
pub const HttpResponse = extern struct {
    status: i32 = 0,
    retry_after: [Limit.retry_after_max]u8 = @splat(0),
    etag: [Limit.etag_max]u8 = @splat(0),
    last_modified: [Limit.http_date_max]u8 = @splat(0),
    content_type: [Limit.content_type_max]u8 = @splat(0),
};

/// `ra8_mdl_storage_iface_t`.
///
/// `validate` is optional: the transfer layer calls it only for a format that
/// has an artifact identity to check.
pub const StorageIface = extern struct {
    begin: ?*const fn (?*anyopaque, ?[*:0]const u8) callconv(.c) u16 = null,
    write: ?*const fn (?*anyopaque, ?[*]const u8, u16, ?*u16) callconv(.c) u16 = null,
    validate: ?*const fn (?*anyopaque, u64, ?[*]const u8) callconv(.c) u16 = null,
    commit: ?*const fn (?*anyopaque) callconv(.c) u16 = null,
    abort: ?*const fn (?*anyopaque) callconv(.c) u16 = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_mdl_sha256_iface_t`.
pub const Sha256Iface = extern struct {
    init: ?*const fn (?*anyopaque) callconv(.c) u16 = null,
    update: ?*const fn (?*anyopaque, ?[*]const u8, u16) callconv(.c) u16 = null,
    final: ?*const fn (?*anyopaque, ?[*]u8) callconv(.c) u16 = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_mdl_session_t`.
pub const Session = extern struct {
    job_id: u32 = 0,
    next_sequence: u32 = 0,
    next_offset: u64 = 0,
    max_chunk_bytes: u32 = 0,
    format: u8 = 0,
    active: bool = false,
};

/// `ra8_mdl_chunk_t`.
pub const Chunk = extern struct {
    job_id: u32 = 0,
    sequence: u32 = 0,
    offset: u64 = 0,
    total_bytes: u64 = 0,
    state: u8 = 0,
    status: u16 = 0,
    data_len: u16 = 0,
    data: [Limit.chunk_data_max]u8 = @splat(0),
    has_sha256: bool = false,
    sha256: [Limit.sha256_bytes]u8 = @splat(0),
    response: HttpResponse = .{},
};

/// `ra8_mdl_request_t`.
pub const Request = extern struct {
    url: ?[*:0]const u8 = null,
    format: u8 = 0,
    http: HttpPolicy = .{},
};

/// `ra8_mdl_transfer_config_t`.
pub const Config = extern struct {
    storage: StorageIface = .{},
    sha256: Sha256Iface = .{},
    format: u8 = 0,
    http: HttpPolicy = .{},
    cancel_requested: ?*const fn (?*anyopaque) callconv(.c) bool = null,
    cancel_ctx: ?*anyopaque = null,
    chunk_bytes: u16 = 0,
    max_chunks: u32 = 0,
};

/// `ra8_mdl_transfer_result_t`.
pub const Result = extern struct {
    bytes_stored: u64 = 0,
    chunks_received: u32 = 0,
    format: u8 = 0,
    sha256: [Limit.sha256_bytes]u8 = @splat(0),
    response: HttpResponse = .{},
};
