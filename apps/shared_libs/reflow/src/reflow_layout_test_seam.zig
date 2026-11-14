//! Test-only link seam for the reflow Zig unit test harness.
//!
//! The layout engine's image sibling (reflow_layout_image.c) sizes `<img>`
//! tokens through `ra8_img_probe_size()`, whose real implementation lives in
//! reflow_image.c behind the ra8_imgdec / ra8_webp / ra8_gfx stack. That stack
//! is out of scope for this unit harness (the CMake suite exercises it end to
//! end), so the harness links the real image and table siblings and satisfies
//! the probe symbol with the documented "not supported" status, which the
//! image sibling handles by falling back to placeholder sizing. The seam must
//! never be imported by reflow_parse_abi.zig: the archive keeps no stub.
const k_ra8_err_not_supported: c_int = 0x107;

export fn ra8_img_probe_size(bytes: [*]const u8, len: usize, out_w: *c_int, out_h: *c_int) callconv(.c) c_int {
    _ = bytes;
    _ = len;
    out_w.* = 0;
    out_h.* = 0;
    return k_ra8_err_not_supported;
}
