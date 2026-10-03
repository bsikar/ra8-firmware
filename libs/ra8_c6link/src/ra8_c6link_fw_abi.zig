//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the firmware-version query: `ra8_c6link_fw_version`, as
//! `ra8_c6link.h` declares it, and the response extractor it hands the RPC
//! layer. The request is built with the vendored codec's own initialisers
//! and issued through `priv_c6link_rpc_call`, which stays in C with the rest
//! of the RPC layer for now.

const std = @import("std");
const Err = @import("abi_err.zig");
const field = @import("ra8_c6link_field_abi.zig");
const header = @import("c6link_rpc_c.zig");

/// The private `ra8_c6link_internal.h` view, codec types included.
pub const c = header.c;

/// The ids this query is issued and answered under.
pub const Id = struct {
    pub const request: u32 = c.RPC_ID__Req_GetCoprocessorFwVersion;
    pub const response: u32 = c.RPC_ID__Resp_GetCoprocessorFwVersion;
};

/// Response extractor: copy every field out for the caller to judge, then
/// record the co-processor's verdict in the fault slot under the request id.
fn takeFw(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    const take: *c.ra8_c6link_take_ctx_t = @ptrCast(@alignCast(ctx.?));
    const msg: *const c.Rpc = @ptrCast(@alignCast(msg_v.?));
    const out: *c.ra8_c6link_fw_version_t = @ptrCast(@alignCast(take.out.?));

    const body: *const c.RpcRespGetCoprocessorFwVersion =
        msg.unnamed_0.resp_get_coprocessor_fwversion orelse return Err.protocol_error;
    out.major = body.major1;
    out.minor = body.minor1;
    out.patch = body.patch1;
    out.chip_id = body.chip_id;
    out.target_len = field.priv_c6link_copy_str(&out.target, out.target.len, @ptrCast(&body.idf_target));
    return c.priv_c6link_resp(take.link, Id.request, body.resp);
}

/// `ra8_c6link_fw_version`: ask the co-processor which firmware it runs.
///
/// `out` is zeroed before the request goes out, so a failed query never
/// leaves a previous answer behind.
pub export fn ra8_c6link_fw_version(link: ?*c.ra8_c6link_t, out: ?*c.ra8_c6link_fw_version_t) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const dst = out orelse return Err.null_ptr;
    if (!handle.open) return Err.not_initialized;
    dst.* = std.mem.zeroes(c.ra8_c6link_fw_version_t);

    var body: c.RpcReqGetCoprocessorFwVersion = undefined;
    c.rpc__req__get_coprocessor_fw_version__init(&body);
    var req: c.Rpc = undefined;
    c.rpc__init(&req);
    req.msg_type = c.RPC_TYPE__Req;
    req.msg_id = c.RPC_ID__Req_GetCoprocessorFwVersion;
    req.payload_case = c.RPC__PAYLOAD_REQ_GET_COPROCESSOR_FWVERSION;
    req.unnamed_0.req_get_coprocessor_fwversion = &body;

    var take = c.ra8_c6link_take_ctx_t{ .link = handle, .out = dst, .rpc_id = 0 };
    return c.priv_c6link_rpc_call(handle, &req, Id.response, takeFw, &take);
}
