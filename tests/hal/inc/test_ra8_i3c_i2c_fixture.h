/**
 * @file test_ra8_i3c_i2c_fixture.h
 * @brief Named addresses, payloads, channels, and budgets for IIC_B tests.
 * @details Keeps protocol fixture constants separate from the test behavior so
 *          the focused test translation unit remains reviewable.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/** @brief Target address, distinct payload bytes, and pre-shifted address bytes. */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_test_target  = 0x50U, /**< 7-bit target address.              */
  k_ra8_i3c_i2c_test_byte_a  = 0xA5U, /**< First TX byte; neither 0 nor 0xFF. */
  k_ra8_i3c_i2c_test_byte_b  = 0x5AU, /**< Its complement: a swap is obvious. */
  k_ra8_i3c_i2c_test_byte_c  = 0x33U, /**< A third, for three-byte writes.    */
  k_ra8_i3c_i2c_test_rx_byte = 0xC3U, /**< A byte unlike every TX fixture.    */
  /** @brief Pre-shifted write-form address. */
  k_ra8_i3c_i2c_test_addr_w = (uint8_t)(k_ra8_i3c_i2c_test_target << 1U),
  /** @brief Pre-shifted read-form address. */
  k_ra8_i3c_i2c_test_addr_r = (uint8_t)((k_ra8_i3c_i2c_test_target << 1U) | 1U),
} ra8_i3c_i2c_test_addr_t;

/** @brief Channel numbers: the one that exists and two invalid selectors. */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_test_ch_zero = 0U,   /**< The only real IIC_B channel on RA8D2. */
  k_ra8_i3c_i2c_test_ch_oor  = 1U,   /**< One past it.                          */
  k_ra8_i3c_i2c_test_ch_huge = 200U, /**< Far beyond the valid channel range.   */
} ra8_i3c_i2c_test_ch_t;

/** @brief Payload length that exhausts the bounded transfer wait. */
typedef enum : uint32_t {
  k_ra8_i3c_i2c_test_long_len = 1000000U, /**< Exceeds the wait budget before access. */
} ra8_i3c_i2c_test_wait_t;
