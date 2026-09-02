/**
 * @file svg_fixture.h
 * @brief Baked deterministic SVG vector-art fixture for ereader_svg.
 *
 * @details A tiny 100x100 vector "cover" exercising the filled-shape paths of
 * the reflow SVG rasterizer (#112/#141): a background `<rect>`, a `<circle>`,
 * a bar `<rect>`, and a `<polygon>` triangle, each a distinct solid colour. SVG
 * is text, so this is a plain ASCII string literal -- no generator needed. The
 * rendered framebuffer hash in hil.conf pins the result.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

/** @brief Baked SVG vector-art document (NUL-terminated ASCII). */
static const char k_svg_fixture[] =
  "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 100\""
  " width=\"100\" height=\"100\">\n"
  "  <rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#202840\"/>\n"
  "  <circle cx=\"50\" cy=\"35\" r=\"22\" fill=\"#E0A018\"/>\n"
  "  <rect x=\"20\" y=\"62\" width=\"60\" height=\"12\" fill=\"#18A030\"/>\n"
  "  <polygon points=\"50,12 62,33 38,33\" fill=\"#C01020\"/>\n"
  "</svg>\n";
