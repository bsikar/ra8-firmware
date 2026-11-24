//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Public C ABI membrane for `libs/ra8_fmt`, exposing the six streaming
//! inspection, conversion, and verification symbols declared in `inc/ra8_fmt_stream.h`.

const c_abi = @import("c_abi");
const inspect = @import("inspect");
const rabook = @import("rabook");
const convert = @import("convert");
const verify_plan = @import("verify_plan");
const verify = @import("verify");

comptime {
    _ = c_abi;
}

/// C ABI export for `ra8_fmt_jof_inspect_stream()`.
pub export fn ra8_fmt_jof_inspect_stream(
    source: ?*const c_abi.Source,
    verbose: bool,
    workspace: ?*const c_abi.JofInspectWorkspace,
    report: ?*const c_abi.Sink,
) callconv(.c) c_abi.Ra8Err {
    return inspect.jofInspectStream(c_abi.live_deps, source, verbose, workspace, report);
}

/// C ABI export for `ra8_fmt_rabook_inspect_stream()`.
pub export fn ra8_fmt_rabook_inspect_stream(
    source: ?*const c_abi.Source,
    verbose: bool,
    workspace: ?*c_abi.RabookInspectWorkspace,
    report: ?*const c_abi.Sink,
) callconv(.c) c_abi.Ra8Err {
    return rabook.rabookInspectStream(c_abi.live_deps, source, verbose, workspace, report);
}

/// C ABI export for `ra8_fmt_jof_convert_requirements()`.
pub export fn ra8_fmt_jof_convert_requirements(
    source: ?*const c_abi.Source,
    out: ?*c_abi.JofConvertRequirements,
) callconv(.c) c_abi.Ra8Err {
    return convert.jofConvertRequirements(c_abi.live_deps, source, out);
}

/// C ABI export for `ra8_fmt_jof_convert_stream()`.
pub export fn ra8_fmt_jof_convert_stream(
    source: ?*const c_abi.Source,
    requirements: ?*const c_abi.JofConvertRequirements,
    workspace: ?*const c_abi.JofConvertWorkspace,
    transaction: ?*c_abi.Transaction,
    report: ?*const c_abi.Sink,
    output_name: ?[*:0]const u8,
) callconv(.c) c_abi.Ra8Err {
    return convert.jofConvertStream(
        c_abi.live_deps,
        source,
        requirements,
        workspace,
        transaction,
        report,
        output_name,
    );
}

/// C ABI export for `ra8_fmt_jof_verify_requirements()`.
pub export fn ra8_fmt_jof_verify_requirements(
    source: ?*const c_abi.Source,
    out: ?*c_abi.JofVerifyRequirements,
) callconv(.c) c_abi.Ra8Err {
    return verify_plan.jofVerifyRequirements(c_abi.live_deps, source, out);
}

/// C ABI export for `ra8_fmt_jof_verify_stream()`.
pub export fn ra8_fmt_jof_verify_stream(
    reference_source: ?*const c_abi.Source,
    banded_source: ?*const c_abi.Source,
    requirements: ?*const c_abi.JofVerifyRequirements,
    workspace: ?*c_abi.JofVerifyWorkspace,
    reference_spool: ?*c_abi.Spool,
    banded_spool: ?*c_abi.Spool,
    dump: ?*c_abi.Transaction,
    dump_name: ?[*:0]const u8,
    report: ?*const c_abi.Sink,
) callconv(.c) c_abi.Ra8Err {
    return verify.jofVerifyStream(
        c_abi.live_deps,
        reference_source,
        banded_source,
        requirements,
        workspace,
        reference_spool,
        banded_spool,
        dump,
        dump_name,
        report,
    );
}
