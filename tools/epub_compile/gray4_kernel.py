# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Deterministic integer gray4 downscale kernel -- host mirror of the firmware.

This is a pure-Python, integer-exact port of the on-device 4-bpp transcode in
``libs/ra8_rabook_compile/src/ra8_rabook_gray4.c``:

  * :func:`gray4_output_dims`  mirrors ``ra8_rabook_gray4_output_dims``.
  * :func:`gray4_downscale`    mirrors ``ra8_rabook_gray4_downscale`` (the Q16.16
                               fixed-point bilinear resample).
  * :func:`gray4_encode`       mirrors ``ra8_rabook_gray4_encode`` (round-to-nearest
                               16-level quantise + two-nibbles-per-byte pack).

Why this file exists (issue #213): the .rabook image pipeline downscales rasters
with the firmware's integer bilinear kernel, but the desktop compiler used to
resample with PIL LANCZOS.  The two kernels produce different pixels, so a
downscaled image was NOT byte-identical host-vs-device.  Downscale is opt-in
(default off, issue #210), yet whenever ``--max-edge`` is requested the divergence
was reachable.  ``epub_compile.py`` now calls this module for the resample step so
the desktop tool and the device emit the same bytes; "green means correct" needs
one deterministic kernel, not a documented exception.

Every integer operation here matches the C exactly, including the truncating
(floor) final shift and the edge-repeat clamp -- there is no floating point.  The
loops are O(dst_w * dst_h); this is fine for the opt-in offline compile and the
tiny parity fixture, and clarity (a 1:1 mirror of the C) is worth more than speed.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

# Q16.16 fixed-point constants (mirror ra8_gray4_fp_t in ra8_rabook_gray4.c).
_FP_SHIFT = 16
_FP_UNIT = 1 << _FP_SHIFT  # 1.0 in Q16.16 (65536).
_FP_MASK = _FP_UNIT - 1  # Fractional-bits mask (low 16 bits).
_FP_NORM_SHIFT = 2 * _FP_SHIFT  # Right-shift to normalise the 64-bit product.
_BYTE_MASK = 0xFF  # (uint8_t) truncation of the normalised sample.

# 16-level even-palette quantise constants (mirror ra8_rabook_gray4_consts_t).
_QUANT_DIV = 17  # Palette step: 255 / 15 == 17.
_ROUND_HALF = 8  # Added before the divide to round-to-nearest.
_NIB_MAX = 15  # Maximum nibble value (4 bits unsigned).
_NIB_SHIFT = 4  # Even pixel occupies the high nibble.
_NIB_PER_BYTE = 2  # Pixels packed per output byte.

# RGB -> gray8 luma coefficients (mirror stbi__compute_y in
# libs/third_party/stb/stb_image.h). The DEVICE decodes every raster with
# stb_image at req_comp=1, so the host MUST fold colour the same way or the same
# source compiles to different .rabook bytes host-vs-device (issue #337). These
# are stb's exact integer weights and its truncating >>8 -- NOT PIL's ITU-R
# 601-2 convert("L"), which rounds and weights green/blue differently and so
# disagrees with the device on ~48% of the RGB cube.
_LUMA_R = 77  # Red weight   (stbi__compute_y).
_LUMA_G = 150  # Green weight (stbi__compute_y).
_LUMA_B = 29  # Blue weight  (stbi__compute_y).
_LUMA_SHIFT = 8  # Normalising right-shift (truncating, matches stb).


def stb_compute_y(r: int, g: int, b: int) -> int:
    """Fold one RGB triple to an 8-bit gray value, byte-identical to stb_image.

    Exact mirror of ``stbi__compute_y`` in ``libs/third_party/stb/stb_image.h``:
    ``(r*77 + g*150 + b*29) >> 8`` with a truncating shift. This is the single
    host luma the device shares; using PIL's ``convert("L")`` here instead would
    diverge from the on-device stb decode on most colour inputs (issue #337).

    Args:
        r: Red channel, 0-255.
        g: Green channel, 0-255.
        b: Blue channel, 0-255.

    Returns:
        The stb gray8 value, 0-255.
    """
    return ((r * _LUMA_R) + (g * _LUMA_G) + (b * _LUMA_B)) >> _LUMA_SHIFT


def gray4_output_dims(src_w: int, src_h: int, max_edge: int) -> tuple[int, int]:
    """Scaled output dims keeping the longer edge within ``max_edge``.

    Exact mirror of ``ra8_rabook_gray4_output_dims``: returns the source dims
    unchanged when they already fit, otherwise scales both by
    ``max_edge / longer_edge`` with round-half-up integer division (matching the C
    ``(dim * max_edge + longer / 2) / longer``), clamping each result to >= 1.

    :param src_w: Source width in pixels.
    :param src_h: Source height in pixels.
    :param max_edge: Maximum allowed length of the longer edge (0 -> (0, 0)).
    :returns: ``(out_w, out_h)``; ``(0, 0)`` when any input is 0.
    """
    if src_w == 0 or src_h == 0 or max_edge == 0:
        return (0, 0)
    longer = max(src_w, src_h)
    if longer <= max_edge:
        return (src_w, src_h)
    half_longer = longer // 2
    out_w = ((src_w * max_edge) + half_longer) // longer
    out_h = ((src_h * max_edge) + half_longer) // longer
    return (max(1, out_w), max(1, out_h))


def _bilinear_sample(src: bytes, src_w: int, src_h: int, sx_fp: int, sy_fp: int) -> int:
    """One bilinear-interpolated sample at Q16.16 source point (sx_fp, sy_fp).

    Exact mirror of ``s_bilinear_sample``: integer bits pick the top-left corner,
    the fractional bits weight the four neighbours, the right/bottom neighbours are
    edge-clamped, and the accumulated 64-bit product is normalised by a truncating
    (floor) right-shift.
    """
    sx0 = sx_fp >> _FP_SHIFT
    sy0 = sy_fp >> _FP_SHIFT
    sx1 = sx0 + 1 if sx0 + 1 < src_w else src_w - 1
    sy1 = sy0 + 1 if sy0 + 1 < src_h else src_h - 1
    fx = sx_fp & _FP_MASK
    fy = sy_fp & _FP_MASK
    ifx = _FP_UNIT - fx
    ify = _FP_UNIT - fy
    row0 = sy0 * src_w
    row1 = sy1 * src_w
    p00 = src[row0 + sx0]
    p10 = src[row0 + sx1]
    p01 = src[row1 + sx0]
    p11 = src[row1 + sx1]
    val = (p00 * ifx * ify) + (p10 * fx * ify) + (p01 * ifx * fy) + (p11 * fx * fy)
    return (val >> _FP_NORM_SHIFT) & _BYTE_MASK


def gray4_downscale(src: bytes, src_w: int, src_h: int, dst_w: int, dst_h: int) -> bytes:
    """Bilinear-resample an 8-bit gray image from (src_w x src_h) to (dst_w x dst_h).

    Exact mirror of ``ra8_rabook_gray4_downscale``: a left-aligned sample grid
    (``sx_fp = dx * ((src_w << 16) // dst_w)``) fed through :func:`_bilinear_sample`.
    An all-zero source yields an all-zero destination, matching the C.

    :param src: Source gray pixels, ``src_w * src_h`` bytes, row-major.
    :param src_w: Source width (>= 0).
    :param src_h: Source height (>= 0).
    :param dst_w: Destination width (> 0).
    :param dst_h: Destination height (> 0).
    :returns: ``dst_w * dst_h`` gray bytes, row-major.
    :raises ValueError: if ``dst_w`` or ``dst_h`` is 0.
    """
    if dst_w == 0 or dst_h == 0:
        msg = "dst_w and dst_h must be > 0"
        raise ValueError(msg)
    if src_w == 0 or src_h == 0:
        return bytes(dst_w * dst_h)
    x_step = (src_w << _FP_SHIFT) // dst_w
    y_step = (src_h << _FP_SHIFT) // dst_h
    out = bytearray(dst_w * dst_h)
    for dy in range(dst_h):
        sy_fp = dy * y_step
        base = dy * dst_w
        for dx in range(dst_w):
            out[base + dx] = _bilinear_sample(src, src_w, src_h, dx * x_step, sy_fp)
    return bytes(out)


def gray4_nibble(value: int) -> int:
    """Quantise one 0-255 gray value to a 0-15 nibble (round-to-nearest, clamped).

    Exact mirror of the per-pixel rule in ``s_pack_nibbles``:
    ``n = min((v + 8) // 17, 15)``.
    """
    nib = (value + _ROUND_HALF) // _QUANT_DIV
    return min(nib, _NIB_MAX)


def gray4_encode(gray: bytes, width: int, height: int) -> bytes:
    """Quantise + nibble-pack a gray buffer to 4-bpp (two pixels per byte).

    Exact mirror of ``ra8_rabook_gray4_encode`` / ``s_pack_nibbles``: even pixels
    land in the high nibble, odd pixels in the low nibble; an odd final pixel keeps
    a zero low half.  Output size is ``(width * height + 1) // 2`` bytes.
    """
    n_pixels = width * height
    out = bytearray((n_pixels + 1) // _NIB_PER_BYTE)
    for i in range(n_pixels):
        nib = gray4_nibble(gray[i])
        byte_idx = i // _NIB_PER_BYTE
        if (i & 1) == 0:
            out[byte_idx] = nib << _NIB_SHIFT
        else:
            out[byte_idx] |= nib
    return bytes(out)


def gray4_transcode(src: bytes, src_w: int, src_h: int, max_edge: int) -> tuple[int, int, bytes]:
    """Full opt-in transcode: output-dims -> downscale -> quantise/pack.

    Convenience wrapper used by the parity generator to reproduce, in one call, the
    exact bytes the device emits for a downscaled raster.  ``max_edge == 0`` (or a
    source already within it) means no resample -- the source pixels are packed as
    is, so the default no-downscale path is covered too.

    :param src: Source gray pixels, ``src_w * src_h`` bytes, row-major.
    :param src_w: Source width in pixels (> 0).
    :param src_h: Source height in pixels (> 0).
    :param max_edge: Opt-in long-edge clamp (0 disables downscaling).
    :returns: ``(out_w, out_h, packed_4bpp_bytes)``.
    """
    out_w, out_h = (src_w, src_h)
    gray = src
    if max_edge != 0:
        out_w, out_h = gray4_output_dims(src_w, src_h, max_edge)
        if (out_w, out_h) != (src_w, src_h):
            gray = gray4_downscale(src, src_w, src_h, out_w, out_h)
    return (out_w, out_h, gray4_encode(gray, out_w, out_h))
