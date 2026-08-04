/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_pin_validator.c
 * @brief Runtime pin-ownership bitmap implementation
 *
 * @details
 * Implements the API defined in `ra8_pin_validator.h`. Uses a fixed
 * 30-byte bitmap (15 ports x 16 pins x 1 bit) and a parallel owner-tag
 * array so that diagnostics can report *which* driver currently owns a
 * given pin.
 */

#include "ra8_pin_validator.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_bit_constants.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"

static const char* s_tag = "PINVAL";

/**
 * @var s_claimed
 * @brief Bitmap of claimed pins (1 bit per port/pin).
 *
 * @details
 * Index = (port * `k_ra8_pin_count`) + pin, then split into
 * `byte = index / k_ra8_bits_per_byte`, `bit = index % k_ra8_bits_per_byte`.
 *
 * @note Size is exactly `k_ra8_port_count * k_ra8_pin_count /
 *       k_ra8_bits_per_byte` bytes = 30.
 */
static uint8_t s_claimed[((uint16_t)k_ra8_port_count * (uint16_t)k_ra8_pin_count) /
                         (uint16_t)k_ra8_bits_per_byte];

/**
 * @var s_owner
 * @brief Owner tag for each claimed pin, indexed by flat (port, pin) index.
 *
 * @details
 * `NULL` means "not claimed". Claim stores the caller-provided string
 * literal pointer; the validator never copies the bytes.
 */
static const char* s_owner[(uint16_t)k_ra8_port_count * (uint16_t)k_ra8_pin_count];

/* Convert a packed `ra8_port_pin_t` to a flat bit index -- see implementation for details. */
RA8_INTERNAL static ra8_err_t internal_flat_index(ra8_port_pin_t pin, uint16_t* out_index)
{
  const uint8_t port = (uint8_t)RA8_PIN_PORT(pin);
  const uint8_t idx  = (uint8_t)RA8_PIN_PIN(pin);

  if (port >= k_ra8_port_count) {
    return k_ra8_err_gpio_invalid_port;
  }
  if (idx >= k_ra8_pin_count) {
    return k_ra8_err_gpio_invalid_pin;
  }

  *out_index = (uint16_t)(((uint16_t)port * (uint16_t)k_ra8_pin_count) + (uint16_t)idx);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_pin_validator_claim()`.
 *
 * @details Marks the pin's bit in `s_claimed` and stores the owner
 *          pointer in `s_owner`.
 *
 * @param[in] pin   Packed port/pin identifier.
 * @param[in] owner String literal naming the claimer; must be non-NULL.
 *
 * @return Error code.
 * @retval k_ra8_ok                     Pin successfully claimed.
 * @retval k_ra8_err_null_ptr           `owner` was NULL.
 * @retval k_ra8_err_gpio_invalid_port  Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin   Pin out of range.
 * @retval k_ra8_err_gpio_conflict      Pin already owned.
 *
 * @pre `ra8_infrastructure_init()` has run.
 * @pre Caller serialises concurrent claim/release calls.
 * @post On success, the pin reads as claimed.
 * @post On failure, validator state is unchanged.
 *
 * @note Not thread-safe with respect to other claim/release callers.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_pin_validator_claim(ra8_port_pin_t pin, const char* owner)
{
  RA8_CHECK_NULL_PTR(owner, s_tag, "owner must not be nullptr");

  uint16_t        flat = 0;
  const ra8_err_t err  = internal_flat_index(pin, &flat);
  if (err != k_ra8_ok) {
    return err;
  }

  const uint16_t byte     = (uint16_t)(flat / k_ra8_bits_per_byte);
  const uint8_t  bit_mask = (uint8_t)(1U << (flat % k_ra8_bits_per_byte));

  if ((s_claimed[byte] & bit_mask) != 0U) {
    ra8_log_error(s_tag, "pin already claimed");
    return k_ra8_err_gpio_conflict;
  }

  s_claimed[byte] |= bit_mask;
  s_owner[flat] = owner;
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_pin_validator_release()`.
 *
 * @details Clears the pin's bit in `s_claimed` and zeroes its owner.
 *
 * @param[in] pin Packed port/pin identifier.
 *
 * @return Error code.
 * @retval k_ra8_ok                     Pin released (or was already free).
 * @retval k_ra8_err_gpio_invalid_port  Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin   Pin out of range.
 *
 * @pre `ra8_infrastructure_init()` has run.
 * @pre Caller serialises concurrent claim/release calls.
 * @post On success, the pin reads as free.
 * @post Owner pointer for the pin is `nullptr`.
 *
 * @note Not thread-safe with respect to other claim/release callers.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_pin_validator_release(ra8_port_pin_t pin)
{
  uint16_t        flat = 0;
  const ra8_err_t err  = internal_flat_index(pin, &flat);
  if (err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE -- tests release validated pins */
    return err;          /* GCOVR_EXCL_LINE                                    */
  }

  const uint16_t byte     = (uint16_t)(flat / k_ra8_bits_per_byte);
  const uint8_t  bit_mask = (uint8_t)(1U << (flat % k_ra8_bits_per_byte));

  s_claimed[byte] &= (uint8_t)~bit_mask;
  s_owner[flat] = nullptr;
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_pin_validator_is_claimed()`.
 *
 * @details Out-of-range identifiers quietly report `false`.
 *
 * @param[in] pin Packed port/pin identifier.
 *
 * @return `true` if claimed, `false` otherwise.
 * @retval true   Pin is owned.
 * @retval false  Pin is free OR identifier is invalid.
 *
 * @pre `ra8_infrastructure_init()` has run.
 * @pre Caller tolerates a best-effort read.
 * @post No state modified.
 * @post Result reflects the bitmap at the moment of the call.
 *
 * @note Thread-safe vs other readers; not safe vs concurrent writers.
 *
 * @since 0.1.0
 */
bool ra8_pin_validator_is_claimed(ra8_port_pin_t pin)
{
  uint16_t        flat = 0;
  const ra8_err_t err  = internal_flat_index(pin, &flat);
  if (err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE -- tests pass valid pins */
    return false;        /* GCOVR_EXCL_LINE                             */
  }

  const uint16_t byte     = (uint16_t)(flat / k_ra8_bits_per_byte);
  const uint8_t  bit_mask = (uint8_t)(1U << (flat % k_ra8_bits_per_byte));
  return (s_claimed[byte] & bit_mask) != 0U;
}

/**
 * @brief Implementation of `ra8_pin_validator_reset()`.
 *
 * @details Zeroes the claim bitmap and the owner array.
 *
 * @pre Called exactly once during early init.
 * @pre No other code is currently inspecting validator state.
 * @post Every pin reads as free.
 * @post Every owner pointer is `nullptr`.
 *
 * @note Not thread-safe; one-shot init only.
 *
 * @since 0.1.0
 */
void ra8_pin_validator_reset(void)
{
  for (uint16_t i = 0U; i < (uint16_t)sizeof(s_claimed); i++) {
    s_claimed[i] = 0U;
  }
  for (uint16_t i = 0U; i < ((uint16_t)k_ra8_port_count * (uint16_t)k_ra8_pin_count); i++) {
    s_owner[i] = nullptr;
  }
}
