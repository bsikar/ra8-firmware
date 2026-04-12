/**
 * @file ra_pin_validator.c
 * @brief Runtime pin-ownership bitmap implementation
 *
 * @details
 * Implements the API defined in `ra_pin_validator.h`. Uses a fixed
 * 30-byte bitmap (15 ports x 16 pins x 1 bit) and a parallel owner-tag
 * array so that diagnostics can report *which* driver currently owns a
 * given pin.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_pin_validator.h"

#include <stdint.h>

#include "ra_bit_constants.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_port_constants.h"

static const char* s_tag = "PINVAL";

/**
 * @var s_claimed
 * @brief Bitmap of claimed pins (1 bit per port/pin).
 *
 * @details
 * Index = (port * `k_ra_pin_count`) + pin, then split into
 * `byte = index / k_ra_bits_per_byte`, `bit = index % k_ra_bits_per_byte`.
 *
 * @note Size is exactly `k_ra_port_count * k_ra_pin_count /
 *       k_ra_bits_per_byte` bytes = 30.
 */
static uint8_t s_claimed[(k_ra_port_count * k_ra_pin_count) / k_ra_bits_per_byte];

/**
 * @var s_owner
 * @brief Owner tag for each claimed pin, indexed by flat (port, pin) index.
 *
 * @details
 * `NULL` means "not claimed". Claim stores the caller-provided string
 * literal pointer; the validator never copies the bytes.
 */
static const char* s_owner[k_ra_port_count * k_ra_pin_count];

/**
 * @brief Convert a packed `ra_port_pin_t` to a flat bit index.
 *
 * @param[in]  pin       Packed port/pin id.
 * @param[out] out_index On success, flat index into `s_claimed` / `s_owner`.
 * @return `k_ra_ok` on valid input, error code otherwise.
 */
static ra_err_t internal_flat_index(ra_port_pin_t pin, uint16_t* out_index)
{
  const uint8_t port = (uint8_t)RA_PIN_PORT(pin);
  const uint8_t idx  = (uint8_t)RA_PIN_PIN(pin);

  if (port >= k_ra_port_count) {
    return k_ra_err_gpio_invalid_port;
  }
  if (idx >= k_ra_pin_count) {
    return k_ra_err_gpio_invalid_pin;
  }

  *out_index = (uint16_t)(((uint16_t)port * (uint16_t)k_ra_pin_count) + (uint16_t)idx);
  return k_ra_ok;
}

ra_err_t ra_pin_validator_claim(ra_port_pin_t pin, const char* owner)
{
  RA_CHECK_NULL_PTR(owner, s_tag, "owner must not be nullptr");

  uint16_t       flat = 0;
  const ra_err_t err  = internal_flat_index(pin, &flat);
  if (err != k_ra_ok) {
    return err;
  }

  const uint16_t byte     = (uint16_t)(flat / k_ra_bits_per_byte);
  const uint8_t  bit_mask = (uint8_t)(1U << (flat % k_ra_bits_per_byte));

  if ((s_claimed[byte] & bit_mask) != 0U) {
    ra_log_error(s_tag, "pin already claimed");
    return k_ra_err_gpio_conflict;
  }

  s_claimed[byte] |= bit_mask;
  s_owner[flat] = owner;
  return k_ra_ok;
}

ra_err_t ra_pin_validator_release(ra_port_pin_t pin)
{
  uint16_t       flat = 0;
  const ra_err_t err  = internal_flat_index(pin, &flat);
  if (err != k_ra_ok) {
    return err;
  }

  const uint16_t byte     = (uint16_t)(flat / k_ra_bits_per_byte);
  const uint8_t  bit_mask = (uint8_t)(1U << (flat % k_ra_bits_per_byte));

  s_claimed[byte] &= (uint8_t)~bit_mask;
  s_owner[flat] = nullptr;
  return k_ra_ok;
}

bool ra_pin_validator_is_claimed(ra_port_pin_t pin)
{
  uint16_t       flat = 0;
  const ra_err_t err  = internal_flat_index(pin, &flat);
  if (err != k_ra_ok) {
    return false;
  }

  const uint16_t byte     = (uint16_t)(flat / k_ra_bits_per_byte);
  const uint8_t  bit_mask = (uint8_t)(1U << (flat % k_ra_bits_per_byte));
  return (s_claimed[byte] & bit_mask) != 0U;
}

void ra_pin_validator_reset(void)
{
  for (uint16_t i = 0U; i < (uint16_t)sizeof(s_claimed); i++) {
    s_claimed[i] = 0U;
  }
  for (uint16_t i = 0U; i < (uint16_t)(k_ra_port_count * k_ra_pin_count); i++) {
    s_owner[i] = nullptr;
  }
}
