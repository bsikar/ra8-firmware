/**
 * @file ra8_dfu_boot.c
 * @brief Pure boot logic for the USB-DFU MRAM bootloader core.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Implements the host-testable half of `ra8_dfu`: software CRC32, image
 * header validation, A/B slot selection, and the reset-time boot
 * decision. No MMIO and no USB -- every function is a pure transform of
 * its arguments, exercised directly by `tests/test_ra8_dfu_boot.c` with
 * MC/DC vectors for each compound decision.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dfu.h"

/**
 * @enum ra8_dfu_crc_const_t
 * @brief IEEE-802.3 reflected-CRC32 parameters (poly / init / final XOR).
 */
typedef enum : uint32_t {
  k_ra8_dfu_crc_init = 0xFFFFFFFFU, /**< Shift-register preset.        */
  k_ra8_dfu_crc_poly = 0xEDB88320U, /**< Reflected poly of 0x04C11DB7. */
  k_ra8_dfu_crc_xout = 0xFFFFFFFFU, /**< Final output XOR.             */
  k_ra8_dfu_crc_lsb  = 0x00000001U, /**< Low-bit test mask.            */
} ra8_dfu_crc_const_t;

/** @brief Bits folded per input byte in the CRC32 inner loop. */
typedef enum : uint8_t {
  k_ra8_dfu_crc_bits = 8U, /**< One byte == eight shift iterations. */
} ra8_dfu_crc_bits_t;

/** @brief Implementation of `ra8_dfu_crc32()` -- bitwise reflected CRC32. */
uint32_t ra8_dfu_crc32(const uint8_t* data, uint32_t len)
{
  uint32_t       crc   = (uint32_t)k_ra8_dfu_crc_init;
  const uint32_t bound = (data == nullptr) ? 0U : len;
  for (uint32_t i = 0U; i < bound; ++i) {
    crc ^= (uint32_t)data[i];
    for (uint8_t b = 0U; b < (uint8_t)k_ra8_dfu_crc_bits; ++b) {
      if ((crc & (uint32_t)k_ra8_dfu_crc_lsb) != 0U) {
        crc = (uint32_t)((crc >> 1U) ^ (uint32_t)k_ra8_dfu_crc_poly);
      } else {
        crc = (uint32_t)(crc >> 1U);
      }
    }
  }
  return (uint32_t)(crc ^ (uint32_t)k_ra8_dfu_crc_xout);
}

bool ra8_dfu_hdr_valid(const ra8_dfu_img_hdr_t* hdr, uint32_t computed_crc)
{
  if (hdr == nullptr) {
    return false;
  }
  const bool magic_ok = (hdr->magic == (uint32_t)k_ra8_dfu_hdr_magic);
  const bool len_ok   = (hdr->img_len != 0U) && (hdr->img_len <= (uint32_t)k_ra8_dfu_img_max) &&
                        ((hdr->img_len % (uint32_t)k_ra8_dfu_page_size) == 0U);
  const bool crc_ok   = (computed_crc == hdr->img_crc32);
  return magic_ok && len_ok && crc_ok;
}

bool ra8_dfu_run_target_valid(uint32_t entry, uint32_t img_len)
{
  const bool entry_ok = (entry == (uint32_t)k_ra8_dfu_run_base);
  const bool len_ok   = (img_len != 0U) && (img_len <= (uint32_t)k_ra8_dfu_img_max) &&
                        ((img_len % (uint32_t)k_ra8_dfu_page_size) == 0U);
  return entry_ok && len_ok;
}

ra8_dfu_slot_t ra8_dfu_select_slot(bool a_valid, uint32_t a_seq, bool b_valid, uint32_t b_seq)
{
  if (!a_valid && !b_valid) {
    return k_ra8_dfu_slot_none;
  }
  if (a_valid && (!b_valid || (a_seq >= b_seq))) {
    return k_ra8_dfu_slot_a;
  }
  return k_ra8_dfu_slot_b;
}

ra8_dfu_action_t
ra8_dfu_boot_decide(bool dfu_trigger, bool a_valid, uint32_t a_seq, bool b_valid, uint32_t b_seq)
{
  if (dfu_trigger) {
    return k_ra8_dfu_action_dfu;
  }
  const ra8_dfu_slot_t slot = ra8_dfu_select_slot(a_valid, a_seq, b_valid, b_seq);
  if (slot == k_ra8_dfu_slot_a) {
    return k_ra8_dfu_action_jump_a;
  }
  if (slot == k_ra8_dfu_slot_b) {
    return k_ra8_dfu_action_jump_b;
  }
  return k_ra8_dfu_action_dfu;
}
