/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file eth_frame_fixture.h
 * @brief Shared test fixture: parse an Ethernet II header out of raw bytes.
 *
 * @details
 * Tests-side companion for the Ethernet suites (`test_ra8_etha.c`) and the
 * `fuzz_ra8_etha` libFuzzer harness: validates that a byte buffer is at
 * least one complete Ethernet II header long, then decodes the destination
 * MAC, source MAC, and big-endian EtherType. It exists so the fuzzer has a
 * deterministic header decode to hammer for out-of-bounds reads and so the
 * unit tests can assert the exact rejection contract (null pointers, short
 * frames) without any driver state.
 *
 * This helper deliberately lives under `tests/` and NOT in the HAL: it was
 * previously compiled into `ra8_etha_stats.c` behind `RA8_OFF_TARGET`
 * (as `ra8_etha_test_inject_rx`), which made the host build carry test-only
 * API surface the firmware never ships. It touches no MMIO and no driver
 * state, so issue #238 moved it here and the driver TU now compiles
 * identically on every build.
 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum eff_layout_t
 * @brief Ethernet II header layout constants for ::eff_parse_eth_header.
 *
 * @details
 * Fixed offsets of an Ethernet II header: 6 bytes destination MAC,
 * 6 bytes source MAC, then the 2-byte big-endian EtherType.
 *
 * @invariant k_eff_hdr_bytes == (2 * k_eff_mac_bytes) + 2.
 * @see eff_parse_eth_header
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_eff_mac_bytes      = 6U,  /**< Bytes per MAC address field.          */
  k_eff_hdr_bytes      = 14U, /**< Complete Ethernet II header length.   */
  k_eff_etype_off_high = 12U, /**< Offset of the EtherType high byte.    */
  k_eff_etype_off_low  = 13U, /**< Offset of the EtherType low byte.     */
  k_eff_byte_shift     = 8U,  /**< Shift to place a byte in bits [15:8]. */
} eff_layout_t;

/**
 * @brief Parse the Ethernet II header at the front of @p frame.
 *
 * @details
 * Performs the minimum sanity check an RX ingestion path would apply
 * (length covers a full 14-byte Ethernet II header), then copies out the
 * destination MAC, source MAC, and the big-endian EtherType. Pure function:
 * no MMIO, no globals, no allocation; on any rejection the output buffers
 * are left untouched.
 *
 * @param[in]  frame     Raw bytes (untrusted). Must not be NULL.
 * @param[in]  frame_len Length in bytes of @p frame.
 * @param[out] out_dst   6-byte destination MAC, populated on success.
 * @param[out] out_src   6-byte source MAC, populated on success.
 * @param[out] out_etype 16-bit big-endian EtherType, populated on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Header parsed; every output populated.
 * @retval k_ra8_err_null_ptr    Any required pointer is NULL.
 * @retval k_ra8_err_invalid_arg @p frame_len is below ::k_eff_hdr_bytes.
 *
 * @pre @p frame points at @p frame_len readable bytes (or is NULL).
 * @pre @p out_dst / @p out_src each hold ::k_eff_mac_bytes writable bytes.
 * @post On k_ra8_ok all three outputs reflect the header fields.
 * @post On any error no output byte has been written.
 *
 * @note Pure function; thread-safe. Test-support code, never linked into
 *       firmware.
 * @since 0.1.0
 */
[[nodiscard]] static inline ra8_err_t eff_parse_eth_header(const uint8_t* frame,
                                                           uint32_t       frame_len,
                                                           uint8_t        out_dst[k_eff_mac_bytes],
                                                           uint8_t        out_src[k_eff_mac_bytes],
                                                           uint16_t*      out_etype)
{
  if (frame == nullptr || out_dst == nullptr || out_src == nullptr || out_etype == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (frame_len < (uint32_t)k_eff_hdr_bytes) {
    return k_ra8_err_invalid_arg;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_eff_mac_bytes; i++) {
    out_dst[i] = frame[i];
    out_src[i] = frame[(uint8_t)k_eff_mac_bytes + i];
  }
  *out_etype = (uint16_t)(((uint16_t)frame[k_eff_etype_off_high] << (uint16_t)k_eff_byte_shift) |
                          (uint16_t)frame[k_eff_etype_off_low]);
  return k_ra8_ok;
}
