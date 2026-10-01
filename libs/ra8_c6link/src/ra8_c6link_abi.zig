//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the Zig half of `ra8_c6link`.
//!
//! The wire layers inside work in slices; this file is the only place raw
//! pointers, declared lengths, out-parameters and `ra8_err_t` codes are
//! handled, so the unchanged declarations in `src/ra8_c6link_internal.h` keep
//! working for the C translation units beside it and the `tests/wireless`
//! suites.

const std = @import("std");

const caps = @import("internal/caps.zig");
const frame = @import("internal/frame.zig");
const mdl_transfer = @import("internal/mdl_transfer.zig");
const mdl_request = @import("internal/mdl_request.zig");
const mdl_chunk = @import("internal/mdl_chunk.zig");
const mdl_service_rules = @import("internal/mdl_service_rules.zig");
const mdl_session = @import("internal/mdl_session.zig");
const mdl_pull = @import("internal/mdl_pull.zig");
const mdl_envelope = @import("internal/mdl_envelope.zig");
const mdl_take = @import("internal/mdl_take.zig");
const mdl_encode = @import("internal/mdl_encode.zig");
const mdl_issue = @import("internal/mdl_issue.zig");
const rpc_wait = @import("internal/rpc_wait.zig");
const sta_cfg = @import("internal/sta_cfg.zig");
const rx_route = @import("internal/rx_route.zig");
const field_copy = @import("internal/field_copy.zig");
const tx_admit = @import("internal/tx_admit.zig");
const wifi_init = @import("internal/wifi_init.zig");
const bare_rpc = @import("internal/bare_rpc.zig");
const sta_policy = @import("internal/sta_policy.zig");
pub const mdl_types = @import("internal/mdl_types.zig");
const storage_ram = @import("internal/storage_ram.zig");
const tlv = @import("internal/tlv.zig");

/// Subset of `ra8_err_t` this library returns.
const Err = struct {
    pub const ok: u16 = 0;
    pub const no_mem: u16 = 0x102;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const invalid_size: u16 = 0x105;
    pub const busy: u16 = 0x109;
    pub const not_initialized: u16 = 0x10F;
    pub const null_ptr: u16 = 0x504;

    /// Flatten one refused storage transition.
    fn of(e: storage_ram.Error) u16 {
        return switch (e) {
            error.InvalidArg => invalid_arg,
            error.InvalidState => invalid_state,
            error.NoMem => no_mem,
        };
    }
};

/// `priv_c6link_tlv_open`: open an envelope for a `proto_len`-byte body.
///
/// Writes both tag headers and the endpoint name into `out`, then reports
/// through `body_at` the offset the protobuf body is to be written at.
pub export fn priv_c6link_tlv_open(
    out: ?[*]u8,
    cap: u16,
    proto_len: u16,
    body_at: ?*u16,
) callconv(.c) u16 {
    const at = body_at orelse return Err.null_ptr;
    const buf = out orelse return Err.null_ptr;
    at.* = 0;

    const offset = tlv.open(buf[0..cap], proto_len) orelse return Err.invalid_size;
    at.* = offset;
    return Err.ok;
}

/// `priv_c6link_tlv_body`: find the protobuf body inside a received envelope.
///
/// Returns null and leaves `proto_len` zero when the payload is not a
/// well-formed envelope addressed to one of the two RPC endpoints.
pub export fn priv_c6link_tlv_body(
    payload: ?[*]const u8,
    len: u16,
    proto_len: ?*u16,
) callconv(.c) ?[*]const u8 {
    const out_len = proto_len orelse return null;
    const buf = payload orelse return null;
    out_len.* = 0;

    const found = tlv.body(buf[0..len]) orelse return null;
    out_len.* = @intCast(found.len);
    return found.ptr;
}

/// `ra8_c6link_frame_class_t`, which the C declares as an `enum : uint8_t`.
const Class = struct {
    pub const data: u8 = 0;
    pub const idle: u8 = 1;
    pub const malformed: u8 = 2;
    pub const bad_checksum: u8 = 3;
};

/// `ra8_c6link_rx_view_t`: where a classified frame's payload is.
const RxView = extern struct {
    offset: u16,
    len: u16,
    if_type: u8,
    if_num: u8,
};

/// `priv_c6link_frame_filler`: write the idle filler transaction.
pub export fn priv_c6link_frame_filler(tx: ?[*]u8) callconv(.c) void {
    const buf = tx orelse return;
    frame.filler(buf[0..frame.Frame.bytes]);
}

/// `priv_c6link_frame_seal`: header a transaction whose payload is staged.
pub export fn priv_c6link_frame_seal(
    tx: ?[*]u8,
    if_type: u8,
    if_num: u8,
    len: u16,
) callconv(.c) void {
    const buf = tx orelse return;
    _ = frame.seal(buf[0..frame.Frame.bytes], if_type, if_num, len);
}

/// `priv_c6link_frame_classify`: decide what a received transaction is.
///
/// Fills `view` only for a data frame, as the C did, so a caller that ignores
/// the verdict cannot read a payload the checksum never covered.
pub export fn priv_c6link_frame_classify(rx: ?[*]const u8, view: ?*RxView) callconv(.c) u8 {
    const buf = rx orelse return Class.malformed;
    const out = view orelse return Class.malformed;

    return switch (frame.classify(buf[0..frame.Frame.bytes])) {
        .idle => Class.idle,
        .malformed => Class.malformed,
        .bad_checksum => Class.bad_checksum,
        .data => |found| {
            out.* = .{
                .offset = found.offset,
                .len = found.len,
                .if_type = found.if_type,
                .if_num = found.if_num,
            };
            return Class.data;
        },
    };
}

/// `priv_c6link_caps`: build the host-capabilities announcement.
pub export fn priv_c6link_caps(out: ?[*]u8, cap: u8) callconv(.c) u8 {
    const buf = out orelse return 0;
    return caps.write(buf[0..cap]) orelse 0;
}

fn storageOf(context: ?*anyopaque) ?*storage_ram.Ram {
    return @ptrCast(@alignCast(context orelse return null));
}

fn ramBegin(context: ?*anyopaque, destination: ?[*:0]const u8) callconv(.c) u16 {
    const label = destination orelse return Err.null_ptr;
    const ram = storageOf(context) orelse return Err.null_ptr;

    ram.begin(std.mem.span(label)) catch |e| return Err.of(e);
    return Err.ok;
}

fn ramWrite(
    context: ?*anyopaque,
    data: ?[*]const u8,
    length: u16,
    written: ?*u16,
) callconv(.c) u16 {
    const out = written orelse return Err.null_ptr;
    const ram = storageOf(context) orelse return Err.null_ptr;
    out.* = 0;

    const bytes = if (data) |p| p[0..length] else if (length == 0)
        &[_]u8{}
    else
        return Err.null_ptr;

    ram.write(bytes) catch |e| return Err.of(e);
    out.* = length;
    return Err.ok;
}

fn ramCommit(context: ?*anyopaque) callconv(.c) u16 {
    const ram = storageOf(context) orelse return Err.null_ptr;
    ram.commit() catch |e| return Err.of(e);
    return Err.ok;
}

fn ramAbort(context: ?*anyopaque) callconv(.c) u16 {
    const ram = storageOf(context) orelse return Err.null_ptr;
    ram.abort() catch |e| return Err.of(e);
    return Err.ok;
}

/// `ra8_mdl_storage_ram_init`: bind an idle adapter and its callback table
/// over `capacity` bytes the caller owns.
pub export fn ra8_mdl_storage_ram_init(
    storage: ?*storage_ram.Ram,
    output: ?*mdl_types.StorageIface,
    data: ?[*]u8,
    capacity: usize,
) callconv(.c) u16 {
    const ram = storage orelse return Err.null_ptr;
    const iface = output orelse return Err.null_ptr;
    const buffer = data orelse return Err.null_ptr;
    if (capacity == 0) return Err.invalid_size;

    ram.* = storage_ram.Ram.bind(buffer[0..capacity]);
    iface.* = .{
        .begin = ramBegin,
        .write = ramWrite,
        .validate = null,
        .commit = ramCommit,
        .abort = ramAbort,
        .ctx = ram,
    };
    return Err.ok;
}

/// `ra8_mdl_storage_ram_view`: hand back the committed extent.
pub export fn ra8_mdl_storage_ram_view(
    storage: ?*const storage_ram.Ram,
    data: ?*?[*]const u8,
    length: ?*usize,
) callconv(.c) u16 {
    const ram = storage orelse return Err.null_ptr;
    const out_data = data orelse return Err.null_ptr;
    const out_len = length orelse return Err.null_ptr;

    const bytes = ram.view() catch |e| return Err.of(e);
    out_data.* = bytes.ptr;
    out_len.* = bytes.len;
    return Err.ok;
}

/// `ra8_c6link_mdl_transfer`: run one complete media transfer.
///
/// The link handle crosses as an opaque pointer: the coordinator hands it
/// straight back to the C media RPC and never reads a field of it, so this
/// slice needs no mirror of `ra8_c6link_t`.
pub export fn ra8_c6link_mdl_transfer(
    link: ?*anyopaque,
    url: ?[*:0]const u8,
    destination: ?[*:0]const u8,
    config: ?*const mdl_types.Config,
    result: ?*mdl_types.Result,
) callconv(.c) u16 {
    if (link == null) return Err.null_ptr;
    const source = url orelse return Err.null_ptr;
    const sink = destination orelse return Err.null_ptr;
    const settings = config orelse return Err.null_ptr;
    const out = result orelse return Err.null_ptr;

    return mdl_transfer.transfer(link, source, sink, settings, out);
}

/// `ra8_c6link_mdl_transfer_commit_test`: the commit half on its own, for the
/// host suites that drive terminal metadata without a transport.
pub export fn ra8_c6link_mdl_transfer_commit_test(
    config: ?*const mdl_types.Config,
    chunk: ?*const mdl_types.Chunk,
    bytes_stored: u64,
    chunks_received: u32,
    result: ?*mdl_types.Result,
) callconv(.c) u16 {
    const settings = config orelse return Err.null_ptr;
    const terminal = chunk orelse return Err.null_ptr;
    const out = result orelse return Err.null_ptr;

    var state = mdl_transfer.State{ .config = settings, .storage_active = true };
    return mdl_transfer.commit(&state, terminal, bytes_stored, chunks_received, out);
}

/// `priv_c6link_mdl_http_field_valid`: one optional HTTP field, checked.
pub export fn priv_c6link_mdl_http_field_valid(
    text: ?[*:0]const u8,
    cap: usize,
) callconv(.c) bool {
    return mdl_request.httpFieldValid(text, cap);
}

/// `priv_c6link_mdl_start_request_valid`: the whole caller argument contract.
///
/// Reports the bounded URL length through `out_url_len` so the encoder copies
/// exactly the length that was checked here rather than measuring caller
/// memory a second time.
pub export fn priv_c6link_mdl_start_request_valid(
    request: ?*const mdl_types.Request,
    out_url_len: ?*usize,
) callconv(.c) u16 {
    const length = mdl_request.startRequestValid(request) catch |err| return switch (err) {
        error.NullPtr => Err.null_ptr,
        error.InvalidArg => Err.invalid_arg,
    };
    const out = out_url_len orelse return Err.null_ptr;
    out.* = length;
    return Err.ok;
}

/// `priv_c6link_mdl_http_response_valid`: terminal HTTP metadata, checked.
pub export fn priv_c6link_mdl_http_response_valid(
    view: ?*const mdl_chunk.View,
) callconv(.c) bool {
    const chunk = view orelse return false;
    return mdl_chunk.httpResponseValid(chunk);
}

/// `priv_c6link_mdl_chunk_semantics_valid`: the state-specific chunk rules.
pub export fn priv_c6link_mdl_chunk_semantics_valid(
    view: ?*const mdl_chunk.View,
) callconv(.c) bool {
    const chunk = view orelse return false;
    return mdl_chunk.semanticsValid(chunk);
}

/// `priv_c6link_mdl_accepted_valid`: does an accepted response open a usable job?
pub export fn priv_c6link_mdl_accepted_valid(
    view: *const mdl_session.AcceptedView,
    requested_format: u32,
) callconv(.c) bool {
    return mdl_session.acceptedValid(view, requested_format);
}

/// `priv_c6link_mdl_session_activate`: open the session an accepted response granted.
pub export fn priv_c6link_mdl_session_activate(
    view: *const mdl_session.AcceptedView,
    session: *mdl_types.Session,
    requested_format: u8,
) callconv(.c) void {
    mdl_session.activate(view, session, requested_format);
}

/// `priv_c6link_mdl_chunk_correlates`: is this chunk the one the session awaits?
pub export fn priv_c6link_mdl_chunk_correlates(
    view: *const mdl_session.ChunkKeyView,
    session: *const mdl_types.Session,
    requested_bytes: u32,
) callconv(.c) bool {
    return mdl_session.chunkCorrelates(view, session, requested_bytes);
}

/// `priv_c6link_mdl_cancelled_valid`: does this acknowledge the job we cancelled?
pub export fn priv_c6link_mdl_cancelled_valid(
    view: *const mdl_session.CancelledView,
    session: *const mdl_types.Session,
) callconv(.c) bool {
    return mdl_session.cancelledValid(view, session);
}

/// `priv_c6link_mdl_session_deactivate`: close a session a cancellation acknowledged.
pub export fn priv_c6link_mdl_session_deactivate(session: *mdl_types.Session) callconv(.c) void {
    mdl_session.deactivate(session);
}

/// `priv_c6link_mdl_cancel_allowed`: may a cancel be issued for this session?
pub export fn priv_c6link_mdl_cancel_allowed(
    session: *const mdl_types.Session,
) callconv(.c) u16 {
    mdl_issue.cancelAllowed(session) catch return Err.invalid_state;
    return Err.ok;
}

/// `priv_c6link_mdl_next_allowed`: may a next request ask for `max_bytes`?
pub export fn priv_c6link_mdl_next_allowed(
    session: *const mdl_types.Session,
    max_bytes: u16,
) callconv(.c) u16 {
    mdl_issue.nextAllowed(session, max_bytes) catch |err| return switch (err) {
        error.InvalidState => Err.invalid_state,
        error.InvalidSize => Err.invalid_size,
    };
    return Err.ok;
}

/// `priv_c6link_mdl_encode_start`: encode a checked StartRequest into the
/// request buffer. `url_len` is the length `priv_c6link_mdl_start_request_valid`
/// reported. False when an argument is NULL or the encode does not fit.
pub export fn priv_c6link_mdl_encode_start(
    request: ?*const mdl_types.Request,
    url_len: usize,
    buf: ?[*]u8,
    capacity: usize,
    out_len: ?*usize,
) callconv(.c) bool {
    const len = out_len orelse return false;
    len.* = 0;
    const req = request orelse return false;
    const dst = (buf orelse return false)[0..capacity];
    const fields = mdl_request.startFields(req, url_len);
    const bytes = mdl_encode.start(dst, fields) catch return false;
    len.* = bytes.len;
    return true;
}

/// `priv_c6link_mdl_encode_next`: encode a NextRequest into the request
/// buffer. False when an argument is NULL or the encode does not fit.
pub export fn priv_c6link_mdl_encode_next(
    job_id: u32,
    acknowledged_offset: u64,
    max_bytes: u32,
    buf: ?[*]u8,
    capacity: usize,
    out_len: ?*usize,
) callconv(.c) bool {
    const len = out_len orelse return false;
    len.* = 0;
    const dst = (buf orelse return false)[0..capacity];
    const bytes = mdl_encode.next(dst, job_id, acknowledged_offset, max_bytes) catch return false;
    len.* = bytes.len;
    return true;
}

/// `priv_c6link_mdl_encode_cancel`: encode a CancelRequest into the request
/// buffer. False when an argument is NULL or the encode does not fit.
pub export fn priv_c6link_mdl_encode_cancel(
    job_id: u32,
    buf: ?[*]u8,
    capacity: usize,
    out_len: ?*usize,
) callconv(.c) bool {
    const len = out_len orelse return false;
    len.* = 0;
    const dst = (buf orelse return false)[0..capacity];
    const bytes = mdl_encode.cancel(dst, job_id) catch return false;
    len.* = bytes.len;
    return true;
}

/// `priv_c6link_mdl_accept_chunk`: copy a validated chunk, advance the session.
///
/// Returns the remote's own status on a FAILED chunk and ok otherwise, which
/// is the value the caller propagates.
pub export fn priv_c6link_mdl_accept_chunk(
    view: ?*const mdl_chunk.View,
    session: ?*mdl_types.Session,
    chunk: ?*mdl_types.Chunk,
) callconv(.c) u16 {
    const decoded = view orelse return Err.null_ptr;
    const active = session orelse return Err.null_ptr;
    const out = chunk orelse return Err.null_ptr;
    return mdl_chunk.accept(decoded, active, out);
}

/// `priv_c6link_mdl_service_field_valid`: one bounded single-line header.
pub export fn priv_c6link_mdl_service_field_valid(
    text: ?[*:0]const u8,
    cap: usize,
) callconv(.c) bool {
    if (cap == 0) return false;
    return mdl_service_rules.fieldValid(text, cap);
}

/// `priv_c6link_mdl_service_response_valid`: fixed terminal response metadata.
pub export fn priv_c6link_mdl_service_response_valid(
    response: ?*const mdl_service_rules.ResponseView,
) callconv(.c) bool {
    const metadata = response orelse return false;
    return mdl_service_rules.responseValid(metadata);
}

/// `priv_c6link_mdl_service_start_valid`: whether a Start request may begin.
pub export fn priv_c6link_mdl_service_start_valid(
    request: ?*const mdl_service_rules.StartView,
) callconv(.c) bool {
    const decoded = request orelse return false;
    return mdl_service_rules.startValid(decoded);
}

/// `priv_c6link_mdl_decode_allocation_fits`: one aligned arena request.
pub export fn priv_c6link_mdl_decode_allocation_fits(
    used: usize,
    len: usize,
    capacity: usize,
) callconv(.c) bool {
    return mdl_service_rules.allocationFits(used, len, capacity);
}

/// `priv_c6link_mdl_decode_aligned_size`: that request's rounded size.
pub export fn priv_c6link_mdl_decode_aligned_size(len: usize) callconv(.c) usize {
    return mdl_service_rules.alignedSize(len);
}

/// `priv_c6link_mdl_service_response_size_ok`: a whole packed response fits.
pub export fn priv_c6link_mdl_service_response_size_ok(
    len: usize,
    response_cap: usize,
) callconv(.c) bool {
    return mdl_service_rules.responseSizeOk(len, response_cap);
}

/// `priv_c6link_mdl_pull_next_correlates`: a NextRequest against the live job.
pub export fn priv_c6link_mdl_pull_next_correlates(
    request: *const mdl_pull.NextRequestView,
    job: *const mdl_pull.JobView,
) bool {
    return mdl_pull.nextCorrelates(request, job);
}

/// `priv_c6link_mdl_pull_cancel_correlates`: a CancelRequest against the job.
pub export fn priv_c6link_mdl_pull_cancel_correlates(
    request: *const mdl_pull.CancelRequestView,
    job: *const mdl_pull.JobView,
) bool {
    return mdl_pull.cancelCorrelates(request, job);
}

/// `priv_c6link_mdl_pull_end_offset`: offset past the body, 0 on overflow.
pub export fn priv_c6link_mdl_pull_end_offset(
    next_offset: u64,
    got: u16,
    overflowed: *bool,
) u64 {
    const end = mdl_pull.endOffset(next_offset, got);
    overflowed.* = end == null;
    return end orelse 0;
}

/// `priv_c6link_mdl_pull_coherent`: whether a backend pull may be packed.
pub export fn priv_c6link_mdl_pull_coherent(view: *const mdl_pull.PullView) bool {
    return mdl_pull.pullCoherent(view);
}

/// `priv_c6link_mdl_pull_advance`: job state after one packed pull.
pub export fn priv_c6link_mdl_pull_advance(
    next_offset: u64,
    next_sequence: u32,
    got: u16,
    complete: bool,
) mdl_pull.Advance {
    return mdl_pull.advance(next_offset, next_sequence, got, complete);
}

/// `priv_c6link_mdl_envelope_operation_valid`: a media operation id.
pub export fn priv_c6link_mdl_envelope_operation_valid(operation: u32) bool {
    return mdl_envelope.operationValid(operation);
}

/// `priv_c6link_mdl_envelope_kind_for`: expected inner response, 0 if none.
pub export fn priv_c6link_mdl_envelope_kind_for(operation: u32) u8 {
    const kind = mdl_envelope.kindFor(operation) orelse return 0;
    return @intFromEnum(kind);
}

/// `priv_c6link_mdl_envelope_accept`: outer response check, 0 when refused.
pub export fn priv_c6link_mdl_envelope_accept(
    view: *const mdl_envelope.ResponseView,
) u8 {
    const kind = mdl_envelope.accept(view) orelse return 0;
    return @intFromEnum(kind);
}

/// `priv_c6link_mdl_take_selected`: the extractor this reply may run, 0 for none.
pub export fn priv_c6link_mdl_take_selected(
    view: *const mdl_envelope.ResponseView,
    expected: u8,
) callconv(.c) u8 {
    const kind = mdl_take.selected(view, expected) orelse return 0;
    return @intFromEnum(kind);
}

/// `priv_c6link_mdl_chunk_admissible`: may this decoded chunk reach the session?
pub export fn priv_c6link_mdl_chunk_admissible(
    key: *const mdl_session.ChunkKeyView,
    view: ?*const mdl_chunk.View,
    session: *const mdl_types.Session,
    requested_bytes: u32,
) callconv(.c) bool {
    const chunk = view orelse return false;
    return mdl_take.chunkAdmissible(key, chunk, session, requested_bytes);
}

/// `priv_c6link_rpc_issuable`: may another request go out on this link?
///
/// Returns `k_ra8_ok`, `k_ra8_err_not_initialized`, or `k_ra8_err_busy`, so
/// the caller returns the verdict as it stands.
pub export fn priv_c6link_rpc_issuable(open: bool, armed: bool, tx_len: u16) callconv(.c) u16 {
    rpc_wait.issuable(open, .{ .uid = 0, .resp_id = 0, .armed = armed }, tx_len) catch |e| {
        return switch (e) {
            error.NotInitialized => Err.not_initialized,
            error.Busy => Err.busy,
        };
    };
    return Err.ok;
}

/// `priv_c6link_rpc_answers`: is this decoded response the outstanding answer?
pub export fn priv_c6link_rpc_answers(
    armed: bool,
    wait_uid: u32,
    wait_resp_id: u32,
    msg_uid: u32,
    msg_id: u32,
) callconv(.c) bool {
    return rpc_wait.answers(
        .{ .uid = wait_uid, .resp_id = wait_resp_id, .armed = armed },
        .{ .uid = msg_uid, .msg_id = msg_id },
    );
}

/// `priv_c6link_sta_len`: measure a credential that may not be terminated.
///
/// Returns `cap` when no terminator was found inside the buffer, which the
/// caller refuses on length rather than reading further.
pub export fn priv_c6link_sta_len(text: ?[*]const u8, cap: u8) callconv(.c) u8 {
    const buf = text orelse return 0;
    return sta_cfg.length(buf[0..cap]);
}

/// `priv_c6link_sta_credentials_valid`: are these one joinable network's lengths?
pub export fn priv_c6link_sta_credentials_valid(ssid_len: u8, pass_len: u8) callconv(.c) bool {
    sta_cfg.credentialsValid(ssid_len, pass_len) catch return false;
    return true;
}

/// `priv_c6link_rx_route`: which consumer this frame's interface number belongs to.
///
/// Returns the `rx_route.Route` ordinal, which `priv_c6link_route_t` mirrors.
pub export fn priv_c6link_rx_route(if_type: u8) callconv(.c) u8 {
    return @intFromEnum(rx_route.routeFor(if_type));
}

/// `priv_c6link_field_take`: octets of a text field that fit a `cap`-octet destination.
pub export fn priv_c6link_field_take(src_len: usize, cap: u8) callconv(.c) usize {
    return field_copy.strTake(src_len, cap);
}

/// `priv_c6link_field_is_mac`: does this field carry exactly one hardware address?
pub export fn priv_c6link_field_is_mac(src_len: usize) callconv(.c) bool {
    return field_copy.macAcceptable(src_len);
}

/// `priv_c6link_tx_admit`: may this Ethernet frame go out right now?
///
/// Returns `k_ra8_ok`, `k_ra8_err_not_initialized`, `k_ra8_err_invalid_size`
/// or `k_ra8_err_busy`, so the caller returns the verdict as it stands.
pub export fn priv_c6link_tx_admit(open: bool, len: u16, tx_len: u16) callconv(.c) u16 {
    tx_admit.admit(open, len, tx_len) catch |e| {
        return switch (e) {
            error.NotInitialized => Err.not_initialized,
            error.InvalidSize => Err.invalid_size,
            error.Busy => Err.busy,
        };
    };
    return Err.ok;
}

/// `priv_c6link_wifi_init_cfg`: the configuration `Req_WifiInit` carries.
///
/// Writes the validated set into @p out, which the caller copies field by
/// field into the generated `WifiInitConfig`.
pub export fn priv_c6link_wifi_init_cfg(out: ?*wifi_init.Cfg) callconv(.c) void {
    const dst = out orelse return;
    dst.* = wifi_init.cfg();
}

/// `priv_c6link_bare_resp`: which answer id pairs with this bare request?
///
/// Writes the `RPC_ID__Resp_*` that answers @p req_id into @p out and returns
/// true. Returns false, leaving @p out at zero, when @p req_id is not one of
/// the requests whose body is empty.
pub export fn priv_c6link_bare_resp(req_id: u32, out: ?*u32) callconv(.c) bool {
    const dst = out orelse return false;
    dst.* = 0;
    const resp = bare_rpc.respFor(req_id) orelse return false;
    dst.* = resp;
    return true;
}

/// `priv_c6link_sta_policy`: the selectors one station join transmits.
///
/// Writes the interface index, scan method, sort order, auth threshold and
/// PMF capability into @p out, which the caller copies into the generated
/// `WifiStaConfig`.
pub export fn priv_c6link_sta_policy(out: ?*sta_policy.Policy) callconv(.c) void {
    const dst = out orelse return;
    dst.* = sta_policy.policy();
}

/// `priv_c6link_sta_bssid_len`: octets of BSSID this join puts on the wire.
pub export fn priv_c6link_sta_bssid_len(pinned: bool) callconv(.c) usize {
    return sta_policy.bssidLen(pinned);
}
