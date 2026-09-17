/**
 * @file fuzz_ra8_jpeg_sw_block.c
 * @brief libFuzzer harness focused on the JPEG block-level Huffman decoder
 *
 * @details
 * Sister harness to ``fuzz_ra8_jpeg_sw.c`` -- where that one mutates the
 * whole JFIF stream, this one prefixes a known-valid JFIF SOI / DQT /
 * DHT / SOF0 / SOS header to the fuzz bytes and lets the libFuzzer
 * mutator concentrate on the entropy-coded segment that drives
 * ``dec_block`` (the per-block DCT-coefficient Huffman decoder
 * declared static in ``libs/ra8_jpeg/src/ra8_jpeg_sw.c``).
 *
 * The fixed header below is the minimum required for a baseline
 * 8x8 grayscale JPEG: one quantisation table, two Huffman tables
 * (DC + AC), one component, and an SOS that selects them. Any byte
 * sequence the fuzzer mutates after the SOS marker becomes the
 * compressed scan data the decoder must walk through ``dec_block``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_jpeg_sw.h"

enum : uint32_t {
  k_fuzz_jpeg_block_max_input  = 16U * 1024U, /**< Fuzz JPEG block maximum input. */
  k_fuzz_jpeg_block_out_pixels = 16U * 16U,   /**< Fuzz JPEG block out pixels.    */
  k_fuzz_jpeg_block_out_bytes =
    k_fuzz_jpeg_block_out_pixels * 3U, /**< Fuzz JPEG block out bytes. */
};

/*
 * Minimal valid baseline 8x8 grayscale JFIF prefix:
 *   SOI                 FF D8
 *   DQT (table 0)       FF DB 00 43 00 [64 bytes of '1']
 *   SOF0 (8x8 1 comp)   FF C0 00 0B 08 00 08 00 08 01 01 11 00
 *   DHT DC table 0      FF C4 00 1F 00 ...
 *   DHT AC table 0      FF C4 00 B5 10 ...
 *   SOS                 FF DA 00 08 01 01 00 00 3F 00
 * After SOS: scan data (mutated by libFuzzer) then EOI (FF D9).
 */
static const uint8_t k_jpeg_prefix[] = {
  /* SOI. */
  0xFFU,
  0xD8U,
  /* DQT (length 67, table id 0, all-ones quant table). */
  0xFFU,
  0xDBU,
  0x00U,
  0x43U,
  0x00U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  1U,
  /* SOF0 (length 11, 8 bpp, 8x8, 1 component, comp1: id=1, h/v=1, qid=0). */
  0xFFU,
  0xC0U,
  0x00U,
  0x0BU,
  0x08U,
  0x00U,
  0x08U,
  0x00U,
  0x08U,
  0x01U,
  0x01U,
  0x11U,
  0x00U,
  /* DHT DC table 0 (canonical baseline DC table from JPEG spec annex K). */
  0xFFU,
  0xC4U,
  0x00U,
  0x1FU,
  0x00U,
  0x00U,
  0x01U,
  0x05U,
  0x01U,
  0x01U,
  0x01U,
  0x01U,
  0x01U,
  0x01U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x01U,
  0x02U,
  0x03U,
  0x04U,
  0x05U,
  0x06U,
  0x07U,
  0x08U,
  0x09U,
  0x0AU,
  0x0BU,
  /* DHT AC table 0 (canonical baseline AC table). */
  0xFFU,
  0xC4U,
  0x00U,
  0xB5U,
  0x10U,
  0x00U,
  0x02U,
  0x01U,
  0x03U,
  0x03U,
  0x02U,
  0x04U,
  0x03U,
  0x05U,
  0x05U,
  0x04U,
  0x04U,
  0x00U,
  0x00U,
  0x01U,
  0x7DU,
  0x01U,
  0x02U,
  0x03U,
  0x00U,
  0x04U,
  0x11U,
  0x05U,
  0x12U,
  0x21U,
  0x31U,
  0x41U,
  0x06U,
  0x13U,
  0x51U,
  0x61U,
  0x07U,
  0x22U,
  0x71U,
  0x14U,
  0x32U,
  0x81U,
  0x91U,
  0xA1U,
  0x08U,
  0x23U,
  0x42U,
  0xB1U,
  0xC1U,
  0x15U,
  0x52U,
  0xD1U,
  0xF0U,
  0x24U,
  0x33U,
  0x62U,
  0x72U,
  0x82U,
  0x09U,
  0x0AU,
  0x16U,
  0x17U,
  0x18U,
  0x19U,
  0x1AU,
  0x25U,
  0x26U,
  0x27U,
  0x28U,
  0x29U,
  0x2AU,
  0x34U,
  0x35U,
  0x36U,
  0x37U,
  0x38U,
  0x39U,
  0x3AU,
  0x43U,
  0x44U,
  0x45U,
  0x46U,
  0x47U,
  0x48U,
  0x49U,
  0x4AU,
  0x53U,
  0x54U,
  0x55U,
  0x56U,
  0x57U,
  0x58U,
  0x59U,
  0x5AU,
  0x63U,
  0x64U,
  0x65U,
  0x66U,
  0x67U,
  0x68U,
  0x69U,
  0x6AU,
  0x73U,
  0x74U,
  0x75U,
  0x76U,
  0x77U,
  0x78U,
  0x79U,
  0x7AU,
  0x83U,
  0x84U,
  0x85U,
  0x86U,
  0x87U,
  0x88U,
  0x89U,
  0x8AU,
  0x92U,
  0x93U,
  0x94U,
  0x95U,
  0x96U,
  0x97U,
  0x98U,
  0x99U,
  0x9AU,
  0xA2U,
  0xA3U,
  0xA4U,
  0xA5U,
  0xA6U,
  0xA7U,
  0xA8U,
  0xA9U,
  0xAAU,
  0xB2U,
  0xB3U,
  0xB4U,
  0xB5U,
  0xB6U,
  0xB7U,
  0xB8U,
  0xB9U,
  0xBAU,
  0xC2U,
  0xC3U,
  0xC4U,
  0xC5U,
  0xC6U,
  0xC7U,
  0xC8U,
  0xC9U,
  0xCAU,
  0xD2U,
  0xD3U,
  0xD4U,
  0xD5U,
  0xD6U,
  0xD7U,
  0xD8U,
  0xD9U,
  0xDAU,
  0xE1U,
  0xE2U,
  0xE3U,
  0xE4U,
  0xE5U,
  0xE6U,
  0xE7U,
  0xE8U,
  0xE9U,
  0xEAU,
  0xF1U,
  0xF2U,
  0xF3U,
  0xF4U,
  0xF5U,
  0xF6U,
  0xF7U,
  0xF8U,
  0xF9U,
  0xFAU,
  /* SOS (length 8, 1 comp, comp1 -> DC table 0 / AC table 0, ss/se/ah/al). */
  0xFFU,
  0xDAU,
  0x00U,
  0x08U,
  0x01U,
  0x01U,
  0x00U,
  0x00U,
  0x3FU,
  0x00U,
};

static const uint8_t k_jpeg_eoi[] = {0xFFU, 0xD9U};

static uint8_t s_buf[k_fuzz_jpeg_block_max_input + sizeof(k_jpeg_prefix) + sizeof(k_jpeg_eoi)];
static uint8_t s_out[k_fuzz_jpeg_block_out_bytes];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (size_t)k_fuzz_jpeg_block_max_input) {
    return 0;
  }
  /* Compose: prefix || fuzz scan data || EOI. */
  size_t pos = 0U;
  memcpy(s_buf + pos, k_jpeg_prefix, sizeof k_jpeg_prefix);
  pos += sizeof k_jpeg_prefix;
  memcpy(s_buf + pos, data, size);
  pos += size;
  memcpy(s_buf + pos, k_jpeg_eoi, sizeof k_jpeg_eoi);
  pos += sizeof k_jpeg_eoi;

  uint16_t out_w = 0U;
  uint16_t out_h = 0U;
  (void)ra8_jpeg_sw_decode(s_buf, (uint32_t)pos, s_out, sizeof s_out, &out_w, &out_h);
  return 0;
}
