/**
 * @file ra_sim_mmio.c
 * @brief Host-side programmable MMIO fault seam
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S} (host test-only)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sim_mmio.h"

#include <stdint.h>

#include "ra_err.h"
#include "ra_hw_err.h" /* prototype for the guarded ra_sim_mmio_wait_eval hook. */

/**
 * @enum ra_sim_mmio_mode_t
 * @brief How an armed register overrides a bounded wait.
 */
typedef enum : uint8_t {
  k_ra_sim_mmio_mode_none          = 0U, /**< Free slot / transparent.    */
  k_ra_sim_mmio_mode_fail          = 1U, /**< Never satisfied -> timeout. */
  k_ra_sim_mmio_mode_satisfy_after = 2U, /**< Satisfied once iter >= arg. */
} ra_sim_mmio_mode_t;

/**
 * @struct ra_sim_mmio_fault_t
 * @brief One armed register entry. A zero @c addr marks a free slot -- an MMIO
 *        register address is never 0 (arming rejects a NULL reg), so 0 is a
 *        safe free-slot sentinel and keeps the lookup a single-condition test.
 */
typedef struct {
  uintptr_t          addr; /**< Polled register address; 0 == free slot. */
  uint32_t           arg;  /**< satisfy-after poll index.                */
  ra_sim_mmio_mode_t mode; /**< Override mode.                           */
} ra_sim_mmio_fault_t;

static ra_sim_mmio_fault_t s_faults[k_ra_sim_mmio_max_faults];

static ra_sim_mmio_fault_t* internal_find(uintptr_t addr)
{
  if (addr == 0U) {
    return nullptr; /* 0 is the free-slot sentinel and the NULL-reg address. */
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra_sim_mmio_max_faults; ++i) {
    if (s_faults[i].addr == addr) {
      return &s_faults[i];
    }
  }
  return nullptr;
}

static ra_err_t internal_arm(const volatile void* reg, ra_sim_mmio_mode_t mode, uint32_t arg)
{
  if (reg == nullptr) {
    return k_ra_err_null_ptr;
  }
  const uintptr_t      addr = (uintptr_t)reg;
  ra_sim_mmio_fault_t* slot = internal_find(addr);
  if (slot == nullptr) {
    for (uint8_t i = 0U; i < (uint8_t)k_ra_sim_mmio_max_faults; ++i) {
      if (s_faults[i].addr == 0U) {
        slot = &s_faults[i];
        break;
      }
    }
  }
  if (slot == nullptr) {
    return k_ra_err_no_mem;
  }
  slot->addr = addr;
  slot->mode = mode;
  slot->arg  = arg;
  return k_ra_ok;
}

void ra_sim_mmio_reset(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_sim_mmio_max_faults; ++i) {
    s_faults[i].addr = 0U;
    s_faults[i].arg  = 0U;
    s_faults[i].mode = k_ra_sim_mmio_mode_none;
  }
}

ra_err_t ra_sim_mmio_fail_wait(const volatile void* reg)
{
  return internal_arm(reg, k_ra_sim_mmio_mode_fail, 0U);
}

ra_err_t ra_sim_mmio_satisfy_after(const volatile void* reg, uint32_t n)
{
  return internal_arm(reg, k_ra_sim_mmio_mode_satisfy_after, n);
}

bool ra_sim_mmio_wait_eval(const volatile void* reg, uint32_t iter, bool real_cond)
{
  const ra_sim_mmio_fault_t* slot = internal_find((uintptr_t)reg);
  if (slot == nullptr) {
    return real_cond; /* transparent: honour the driver's real RAM poll. */
  }
  if (slot->mode == k_ra_sim_mmio_mode_satisfy_after) {
    return (iter >= slot->arg);
  }
  return false; /* k_ra_sim_mmio_mode_fail: never satisfied -> caller times out. */
}
